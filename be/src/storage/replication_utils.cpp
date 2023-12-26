// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage/replication_utils.h"

#include <sys/stat.h>

#ifdef BE_TEST
#include "agent/agent_server.h"
#endif

#include "fs/fs.h"
#include "fs/fs_util.h"
#include "gen_cpp/BackendService.h"
#include "gen_cpp/Types_constants.h"
#include "gutil/strings/split.h"
#include "gutil/strings/stringpiece.h"
#include "gutil/strings/substitute.h"
#include "http/http_client.h"
#include "runtime/client_cache.h"
#include "service/backend_options.h"
#include "util/string_parser.hpp"
#include "util/thrift_rpc_helper.h"

namespace starrocks {

const std::string HTTP_REQUEST_PREFIX = "/api/_tablet/_download";
const uint32_t DOWNLOAD_FILE_MAX_RETRY = 3;
const uint32_t LIST_REMOTE_FILE_TIMEOUT = 15;
const uint32_t GET_LENGTH_TIMEOUT = 10;

#ifndef BE_TEST
static Status list_remote_files(const std::string& remote_url_prefix, std::vector<string>* file_name_list,
                                std::vector<int64_t>* file_size_list) {
    // Get remote dir file list
    string file_list_str;
    auto list_files_cb = [&remote_url_prefix, &file_list_str](HttpClient* client) {
        RETURN_IF_ERROR(client->init(remote_url_prefix));
        client->set_timeout_ms(LIST_REMOTE_FILE_TIMEOUT * 1000);
        RETURN_IF_ERROR(client->execute(&file_list_str));
        return Status::OK();
    };
    RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, list_files_cb));

    // Parse file name and size
    const char* const FILE_DELIMETER_IN_DIR_RESPONSE = "\n";
    const char* const FILE_NAME_SIZE_DELIMETER = "|";

    bool use_file_name_and_size_format = file_list_str.find(FILE_NAME_SIZE_DELIMETER) != string::npos;
    if (use_file_name_and_size_format) {
        for (auto file_str : strings::Split(file_list_str, FILE_DELIMETER_IN_DIR_RESPONSE, strings::SkipWhitespace())) {
            std::vector<string> list = strings::Split(file_str, FILE_NAME_SIZE_DELIMETER);
            if (list.size() != 2) {
                return Status::InternalError(fmt::format("invalid directory entry {}", file_str.as_string()));
            }

            StringParser::ParseResult result;
            std::string& file_size_str = list[1];
            auto file_size = StringParser::string_to_int<int64_t>(file_size_str.data(), file_size_str.size(), &result);
            if (result != StringParser::PARSE_SUCCESS || file_size < 0) {
                return Status::InternalError("wrong file size.");
            }

            file_name_list->emplace_back(std::move(list[0]));
            file_size_list->emplace_back(file_size);
        }
    } else {
        *file_name_list = strings::Split(file_list_str, FILE_DELIMETER_IN_DIR_RESPONSE, strings::SkipWhitespace());
    }
    return Status::OK();
}

static StatusOr<uint64_t> get_remote_file_size(const std::string& remote_file_url) {
    uint64_t file_size = 0;
    auto get_file_size_cb = [&remote_file_url, &file_size](HttpClient* client) {
        RETURN_IF_ERROR(client->init(remote_file_url));
        client->set_timeout_ms(GET_LENGTH_TIMEOUT * 1000);
        RETURN_IF_ERROR(client->head());
        file_size = client->get_content_length();
        return Status::OK();
    };
    RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, get_file_size_cb));
    return file_size;
}

static StatusOr<uint64_t> download_remote_file(const std::string& remote_file_url, const std::string& local_file_path,
                                               uint64_t timeout_sec) {
    uint64_t file_size = 0;
    auto download_cb = [&remote_file_url, timeout_sec, &local_file_path, &file_size](HttpClient* client) {
        RETURN_IF_ERROR(client->init(remote_file_url));
        client->set_timeout_ms(timeout_sec * 1000);
        ASSIGN_OR_RETURN(file_size, client->download(local_file_path));
        return Status::OK();
    };
    RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, download_cb));
    return file_size;
}
#endif

Status ReplicationUtils::make_remote_snapshot(const std::string& host, int32_t be_port, TTabletId tablet_id,
                                              TSchemaHash schema_hash, TVersion version, int32_t timeout_s,
                                              const std::vector<Version>* missed_versions,
                                              const std::vector<int64_t>* missing_version_ranges,
                                              std::string* remote_snapshot_path) {
    TSnapshotRequest request;
    request.__set_tablet_id(tablet_id);
    request.__set_schema_hash(schema_hash);
    request.__set_preferred_snapshot_format(g_Types_constants.TPREFER_SNAPSHOT_REQ_VERSION);
    if (missed_versions != nullptr) {
        DCHECK(!missed_versions->empty());
        request.__isset.missing_version = true;
        for (auto& version : *missed_versions) {
            // NOTE: assume missing version composed of singleton delta.
            DCHECK_EQ(version.first, version.second);
            request.missing_version.push_back(version.first);
        }
    }
    if (missing_version_ranges != nullptr) {
        DCHECK(!missing_version_ranges->empty());
        request.__isset.missing_version_ranges = true;
        for (auto v : *missing_version_ranges) {
            request.missing_version_ranges.push_back(v);
        }
    }
    if (version > 0) {
        request.__set_version(version);
    }
    if (timeout_s > 0) {
        request.__set_timeout(timeout_s);
    }

    TAgentResult result;

#ifdef BE_TEST
    ExecEnv::GetInstance()->agent_server()->make_snapshot(result, request);
#else
    // snapshot will hard link all required rowsets' segment files, the number of files may be very large(>1000),
    // so it may take some time to process this rpc, so we increase rpc timeout from 5s to 20s to reduce the chance
    // of timeout for now, we may need a smart way to estimate the time of make_snapshot in future
    RETURN_IF_ERROR(ThriftRpcHelper::rpc<BackendServiceClient>(
            host, be_port,
            [&request, &result](BackendServiceConnection& client) { client->make_snapshot(result, request); },
            config::make_snapshot_rpc_timeout_ms));
#endif

    if (result.status.status_code != TStatusCode::OK) {
        return {result.status};
    }

    if (result.__isset.snapshot_path) {
        *remote_snapshot_path = result.snapshot_path;
        if (remote_snapshot_path->at(remote_snapshot_path->length() - 1) != '/') {
            remote_snapshot_path->append("/");
        }
    } else {
        return Status::InternalError("success snapshot without snapshot path");
    }

    if (result.snapshot_format != g_Types_constants.TSNAPSHOT_REQ_VERSION2) {
        LOG(WARNING) << "Unsupported snapshot format version: " << result.snapshot_format << ", from: " << host
                     << ", tablet: " << tablet_id;
        return Status::NotSupported("Unsupported snapshot format version");
    }

    return Status::OK();
}

Status ReplicationUtils::release_remote_snapshot(const std::string& ip, int32_t port,
                                                 const std::string& src_snapshot_path) {
    TAgentResult result;

#ifdef BE_TEST
    ExecEnv::GetInstance()->agent_server()->release_snapshot(result, src_snapshot_path);
#else
    RETURN_IF_ERROR(ThriftRpcHelper::rpc<BackendServiceClient>(
            ip, port, [&src_snapshot_path, &result](BackendServiceConnection& client) {
                client->release_snapshot(result, src_snapshot_path);
            }));
#endif
    return {result.status};
}

Status ReplicationUtils::download_remote_snapshot(
        const std::string& host, int32_t http_port, const std::string& remote_token,
        const std::string& remote_snapshot_path, TTabletId remote_tablet_id, TSchemaHash remote_schema_hash,
        DataDir* data_dir, const std::string& local_path_prefix,
        const std::function<std::string(const std::string&)>& name_converter) {
#ifdef BE_TEST
    std::error_code error_code;
    std::filesystem::copy(strings::Substitute("$0/$1/$2/", remote_snapshot_path, remote_tablet_id, remote_schema_hash),
                          local_path_prefix, error_code);
    if (error_code) {
        return Status::InternalError(error_code.message());
    }
    return Status::OK();
#else

    std::string remote_url_prefix =
            strings::Substitute("http://$0:$1$2?token=$3&type=V2&file=$4/$5/$6/", host, http_port, HTTP_REQUEST_PREFIX,
                                remote_token, remote_snapshot_path, remote_tablet_id, remote_schema_hash);

    std::vector<string> file_name_list;
    std::vector<int64_t> file_size_list;
    RETURN_IF_ERROR(list_remote_files(remote_url_prefix, &file_name_list, &file_size_list));

    // If the header file is not exist, the table could't loaded by olap engine.
    // Avoid of data is not complete, we copy the header file at last.
    // The header file's name is end of .hdr.
    for (int i = 0; i < file_name_list.size() - 1; ++i) {
        StringPiece sp(file_name_list[i]);
        if (sp.ends_with(".hdr")) {
            std::swap(file_name_list[i], file_name_list[file_name_list.size() - 1]);
            if (!file_size_list.empty()) {
                std::swap(file_size_list[i], file_size_list[file_size_list.size() - 1]);
            }
            break;
        }
    }

    // Get copy from remote
    uint64_t total_file_size = 0;
    uint64_t skipped_file_count = 0;
    MonotonicStopWatch watch;
    watch.start();
    for (int i = 0; i < file_name_list.size(); ++i) {
        const std::string& remote_file_name = file_name_list[i];
        auto remote_file_url = remote_url_prefix + remote_file_name;

        uint64_t file_size = 0;
        if (!file_size_list.empty()) {
            file_size = file_size_list[i];
        } else {
            ASSIGN_OR_RETURN(file_size, get_remote_file_size(remote_file_url));
        }

        // check disk capacity
        if (data_dir != nullptr && data_dir->capacity_limit_reached(file_size)) {
            return Status::InternalError("Disk reach capacity limit");
        }

        total_file_size += file_size;
        uint64_t estimate_timeout = file_size / config::download_low_speed_limit_kbps / 1024;
        if (estimate_timeout < config::download_low_speed_time) {
            estimate_timeout = config::download_low_speed_time;
        }

        std::string local_file_name = name_converter ? name_converter(remote_file_name) : remote_file_name;
        if (local_file_name.empty()) {
            ++skipped_file_count;
            LOG(INFO) << "Skipped download remote file: " << remote_file_url << ", file_size: " << file_size;
            continue;
        }

        std::string local_file_path = local_path_prefix + local_file_name;

        VLOG(1) << "Downloading " << remote_file_url << " to " << local_file_path << ", bytes: " << file_size
                << ", timeout: " << estimate_timeout;

        ASSIGN_OR_RETURN(uint64_t local_file_size,
                         download_remote_file(remote_file_url, local_file_path, estimate_timeout));
        // Check file length
        if (local_file_size != file_size) {
            LOG(WARNING) << "Fail to download " << remote_file_url << ", file_size: " << local_file_size << "/"
                         << file_size;
            return Status::InternalError("mismatched file size");
        }
    } // Clone files from remote backend

    uint64_t total_time_ms = watch.elapsed_time() / 1000 / 1000;
    total_time_ms = total_time_ms > 0 ? total_time_ms : 0;
    double copy_rate = 0.0;
    if (total_time_ms > 0) {
        copy_rate = total_file_size / ((double)total_time_ms) / 1000;
    }
    LOG(INFO) << "Copied tablet file count: " << (file_name_list.size() - skipped_file_count)
              << ", skipped: " << skipped_file_count << ", bytes: " << total_file_size << ", cost: " << total_time_ms
              << " ms, rate: " << copy_rate << " MB/s";
    return Status::OK();
#endif
}

StatusOr<std::string> ReplicationUtils::download_remote_snapshot_file(
        const std::string& host, int32_t http_port, const std::string& remote_token,
        const std::string& remote_snapshot_path, TTabletId remote_tablet_id, TSchemaHash remote_schema_hash,
        const std::string& file_name, uint64_t timeout_sec) {
#ifdef BE_TEST
    std::string path =
            strings::Substitute("$0/$1/$2/$3", remote_snapshot_path, remote_tablet_id, remote_schema_hash, file_name);
    ASSIGN_OR_RETURN(auto file, fs::new_random_access_file(path));
    return file->read_all();
#else

    std::string remote_file_url = strings::Substitute(
            "http://$0:$1$2?token=$3&type=V2&file=$4/$5/$6/$7", host, http_port, HTTP_REQUEST_PREFIX, remote_token,
            remote_snapshot_path, remote_tablet_id, remote_schema_hash, file_name);

    std::string file_content;
    auto download_cb = [&remote_file_url, timeout_sec, &file_content](HttpClient* client) {
        RETURN_IF_ERROR(client->init(remote_file_url));
        client->set_timeout_ms(timeout_sec * 1000);
        ASSIGN_OR_RETURN(file_content, client->download());
        return Status::OK();
    };
    RETURN_IF_ERROR(HttpClient::execute_with_retry(DOWNLOAD_FILE_MAX_RETRY, 1, download_cb));
    return file_content;
#endif
}

} // namespace starrocks
