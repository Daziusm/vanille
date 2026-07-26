#pragma once

#include "lua/instance.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sandbox
{
    class data_model : public instance
    {
    public:
        data_model();

        void add_service(const std::shared_ptr<instance>& service);
        std::shared_ptr<instance> get_service(const std::string& service_name) const;
        std::vector<std::shared_ptr<instance>> get_services() const;

    private:
        static std::string normalize_service_name(const std::string& value);

        mutable std::mutex service_mutex_;
        std::unordered_map<std::string, std::shared_ptr<instance>> service_map_;
    };

    std::shared_ptr<data_model> create_default_data_model();
}
