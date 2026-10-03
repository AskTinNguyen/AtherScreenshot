#pragma once
#include "common.h"

namespace ather {

struct UploadConfig {
    std::wstring uploader;  // imgur | custom | s3
    std::wstring imgurClientId;
    std::wstring customUrl, customFileField, customHeaders, customResponseUrl;
    std::wstring s3Endpoint, s3Bucket, s3Region, s3AccessKey, s3SecretKey, s3PublicUrl;
};

// Validates the configuration; returns an explanation if it can't work.
std::wstring UploadConfigProblem(const UploadConfig& cfg);
// Uploads `path` on a worker thread; `done` runs on the UI thread with the public URL or an error.
void UploadFileAsync(const std::wstring& path, const UploadConfig& cfg,
                     std::function<void(std::wstring url, std::wstring error)> done);

// Exposed for reuse/tests: dotted JSON path lookup ("data.link", "files.0.url") on a UTF-8 body.
std::wstring JsonPath(const std::string& json, const std::wstring& path);

}  // namespace ather
