#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sandbox
{
    using attribute_value = std::variant<std::monostate, bool, double, std::string>;

    class instance : public std::enable_shared_from_this<instance>
    {
    public:
        instance(std::string class_name, std::string name = std::string());
        virtual ~instance() = default;

        std::string get_name() const;
        void set_name(const std::string& name);

        std::string get_class_name() const;

        std::shared_ptr<instance> get_parent() const;
        bool set_parent(const std::shared_ptr<instance>& parent);

        std::vector<std::shared_ptr<instance>> get_children() const;
        std::vector<std::shared_ptr<instance>> get_descendants() const;
        std::shared_ptr<instance> find_first_child(const std::string& name) const;
        std::shared_ptr<instance> find_first_descendant(const std::string& name) const;
        bool is_descendant_of(const std::shared_ptr<instance>& ancestor) const;
        bool is_ancestor_of(const std::shared_ptr<instance>& descendant) const;
        std::shared_ptr<instance> wait_for_child(const std::string& name, double timeout_seconds);

        std::optional<attribute_value> get_attribute(const std::string& key) const;
        void set_attribute(const std::string& key, const attribute_value& value);
        std::unordered_map<std::string, attribute_value> get_attributes() const;

    private:
        bool would_create_cycle(const std::shared_ptr<instance>& new_parent) const;
        void add_child_internal(const std::shared_ptr<instance>& child);
        void remove_child_internal(instance* child);

        mutable std::mutex mutex_;
        std::condition_variable children_cv_;
        std::string name_;
        std::string class_name_;
        std::weak_ptr<instance> parent_;
        std::vector<std::shared_ptr<instance>> children_;
        std::unordered_map<std::string, attribute_value> attributes_;
    };
}
