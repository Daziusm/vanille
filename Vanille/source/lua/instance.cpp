#include "lua/instance.h"

#include <algorithm>
#include <chrono>

namespace sandbox
{
    namespace
    {
        std::string normalize_class_name(const std::string& class_name)
        {
            if (class_name.empty())
            {
                return "Instance";
            }
            return class_name;
        }

        std::string normalize_name(const std::string& name, const std::string& class_name)
        {
            if (name.empty())
            {
                return class_name;
            }
            return name;
        }
    }

    instance::instance(std::string class_name, std::string name)
        : class_name_(normalize_class_name(class_name)),
        name_(normalize_name(name, class_name_))
    {
    }

    std::string instance::get_name() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return name_;
    }

    void instance::set_name(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        name_ = normalize_name(name, class_name_);
    }

    std::string instance::get_class_name() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return class_name_;
    }

    std::shared_ptr<instance> instance::get_parent() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return parent_.lock();
    }

    bool instance::would_create_cycle(const std::shared_ptr<instance>& new_parent) const
    {
        std::shared_ptr<instance> current = new_parent;
        while (current)
        {
            if (current.get() == this)
            {
                return true;
            }
            current = current->get_parent();
        }
        return false;
    }

    void instance::add_child_internal(const std::shared_ptr<instance>& child)
    {
        if (!child)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = std::find_if(children_.begin(), children_.end(), [&](const std::shared_ptr<instance>& item)
            {
                return item.get() == child.get();
            });
        if (iterator == children_.end())
        {
            children_.push_back(child);
            children_cv_.notify_all();
        }
    }

    void instance::remove_child_internal(instance* child)
    {
        if (!child)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = std::remove_if(children_.begin(), children_.end(), [&](const std::shared_ptr<instance>& item)
            {
                return item.get() == child;
            });
        if (iterator != children_.end())
        {
            children_.erase(iterator, children_.end());
            children_cv_.notify_all();
        }
    }

    bool instance::set_parent(const std::shared_ptr<instance>& parent)
    {
        if (parent.get() == this)
        {
            return false;
        }

        if (would_create_cycle(parent))
        {
            return false;
        }

        std::shared_ptr<instance> self = shared_from_this();
        std::shared_ptr<instance> old_parent;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            old_parent = parent_.lock();
            if (old_parent.get() == parent.get())
            {
                return true;
            }
            parent_ = parent;
        }

        if (old_parent)
        {
            old_parent->remove_child_internal(this);
        }

        if (parent)
        {
            parent->add_child_internal(self);
        }

        return true;
    }

    std::vector<std::shared_ptr<instance>> instance::get_children() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return children_;
    }

    std::vector<std::shared_ptr<instance>> instance::get_descendants() const
    {
        std::vector<std::shared_ptr<instance>> output;
        std::vector<std::shared_ptr<instance>> pending = get_children();
        while (!pending.empty())
        {
            std::shared_ptr<instance> current = pending.back();
            pending.pop_back();
            if (!current)
            {
                continue;
            }
            output.push_back(current);
            std::vector<std::shared_ptr<instance>> children = current->get_children();
            pending.insert(pending.end(), children.begin(), children.end());
        }
        return output;
    }

    std::shared_ptr<instance> instance::find_first_child(const std::string& name) const
    {
        std::vector<std::shared_ptr<instance>> children = get_children();
        for (const std::shared_ptr<instance>& child : children)
        {
            if (!child)
            {
                continue;
            }

            if (child->get_name() == name)
            {
                return child;
            }
        }
        return nullptr;
    }

    std::shared_ptr<instance> instance::find_first_descendant(const std::string& name) const
    {
        std::vector<std::shared_ptr<instance>> descendants = get_descendants();
        for (const std::shared_ptr<instance>& descendant : descendants)
        {
            if (!descendant)
            {
                continue;
            }

            if (descendant->get_name() == name)
            {
                return descendant;
            }
        }
        return nullptr;
    }

    bool instance::is_descendant_of(const std::shared_ptr<instance>& ancestor) const
    {
        if (!ancestor)
        {
            return false;
        }

        std::shared_ptr<instance> current = get_parent();
        while (current)
        {
            if (current.get() == ancestor.get())
            {
                return true;
            }
            current = current->get_parent();
        }
        return false;
    }

    bool instance::is_ancestor_of(const std::shared_ptr<instance>& descendant) const
    {
        if (!descendant)
        {
            return false;
        }
        return descendant->is_descendant_of(const_cast<instance*>(this)->shared_from_this());
    }

    std::shared_ptr<instance> instance::wait_for_child(const std::string& name, double timeout_seconds)
    {
        if (const std::shared_ptr<instance> existing = find_first_child(name))
        {
            return existing;
        }

        if (timeout_seconds == 0.0)
        {
            return nullptr;
        }

        std::shared_ptr<instance> found;
        auto find_locked = [&]() -> std::shared_ptr<instance>
        {
            for (const std::shared_ptr<instance>& child : children_)
            {
                if (child && child->get_name() == name)
                {
                    return child;
                }
            }
            return nullptr;
        };

        std::unique_lock<std::mutex> lock(mutex_);
        found = find_locked();
        if (found)
        {
            return found;
        }

        if (timeout_seconds < 0.0)
        {
            children_cv_.wait(lock, [&]()
                {
                    found = find_locked();
                    return static_cast<bool>(found);
                });
            return found;
        }

        const std::chrono::duration<double> timeout_duration(timeout_seconds);
        children_cv_.wait_for(lock, timeout_duration, [&]()
            {
                found = find_locked();
                return static_cast<bool>(found);
            });

        return found;
    }

    std::optional<attribute_value> instance::get_attribute(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = attributes_.find(key);
        if (iterator == attributes_.end())
        {
            return std::nullopt;
        }
        return iterator->second;
    }

    void instance::set_attribute(const std::string& key, const attribute_value& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<std::monostate>(value))
        {
            attributes_.erase(key);
        }
        else
        {
            attributes_[key] = value;
        }
    }

    std::unordered_map<std::string, attribute_value> instance::get_attributes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return attributes_;
    }
}
