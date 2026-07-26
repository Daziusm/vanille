#pragma once

#include <functional>
#include <string>

namespace downloader
{
    struct github_target
    {
        std::wstring owner;
        std::wstring repo;
        std::wstring branch;
    };

    bool parse_github_url(const std::wstring& url, github_target& out);
    bool download_repository_zip(const github_target& target, const std::wstring& zip_path, std::wstring& out_error);
    bool extract_zip_to_directory(const std::wstring& zip_path, const std::wstring& destination, std::wstring& out_error);
    bool install_sources(
        const std::wstring& repo_url,
        const std::wstring& branch,
        const std::wstring& destination,
        std::wstring& out_error);
}
