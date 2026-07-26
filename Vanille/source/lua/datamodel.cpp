#include "lua/datamodel.h"

#include "lua/services_lighting.h"
#include "lua/services_players.h"
#include "lua/services_run_service.h"
#include "lua/services_user_input_service.h"
#include "lua/services_workspace.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace sandbox
{
    data_model::data_model()
        : instance("DataModel", "game")
    {
    }

    std::string data_model::normalize_service_name(const std::string& value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        for (unsigned char character : value)
        {
            if (std::isalnum(character))
            {
                normalized.push_back(static_cast<char>(std::tolower(character)));
            }
        }
        return normalized;
    }

    void data_model::add_service(const std::shared_ptr<instance>& service)
    {
        if (!service)
        {
            return;
        }

        const std::string name_key = normalize_service_name(service->get_name());
        const std::string class_key = normalize_service_name(service->get_class_name());

        service->set_parent(shared_from_this());

        std::lock_guard<std::mutex> lock(service_mutex_);
        if (!name_key.empty())
        {
            service_map_[name_key] = service;
        }
        if (!class_key.empty())
        {
            service_map_[class_key] = service;
        }
    }

    std::shared_ptr<instance> data_model::get_service(const std::string& service_name) const
    {
        const std::string normalized = normalize_service_name(service_name);
        if (normalized.empty())
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(service_mutex_);
        const auto iterator = service_map_.find(normalized);
        if (iterator == service_map_.end())
        {
            return nullptr;
        }

        return iterator->second;
    }

    std::vector<std::shared_ptr<instance>> data_model::get_services() const
    {
        std::vector<std::shared_ptr<instance>> services;
        std::unordered_set<const instance*> seen;

        std::lock_guard<std::mutex> lock(service_mutex_);
        for (const auto& pair : service_map_)
        {
            if (!pair.second)
            {
                continue;
            }

            const instance* pointer = pair.second.get();
            if (seen.insert(pointer).second)
            {
                services.push_back(pair.second);
            }
        }

        std::sort(services.begin(), services.end(), [](const std::shared_ptr<instance>& left, const std::shared_ptr<instance>& right)
            {
                return left->get_name() < right->get_name();
            });

        return services;
    }

    std::shared_ptr<data_model> create_default_data_model()
    {
        const std::shared_ptr<data_model> model = std::make_shared<data_model>();
        model->add_service(create_workspace_service());
        model->add_service(create_players_service());
        model->add_service(create_run_service());
        model->add_service(create_user_input_service());
        model->add_service(create_lighting_service());
        return model;
    }
}
