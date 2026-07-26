#pragma once

#include <string>
#include <functional>

namespace payload
{
    // Extract embedded payload to the install directory when missing or outdated.
    bool ensure_installed(std::wstring& out_runtime_dir, const std::wstring& preferred_install_dir = {});

    bool launch_vanille(const std::wstring& runtime_dir);

    void set_status_sink(std::function<void(const std::wstring&)> sink);
}
