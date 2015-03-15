/*
 * ===============================================================
 *    Description:  db::element implementation.
 *
 *        Created:  2014-05-30 16:58:32
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#include "common/event_order.h"
#include "db/element.h"

using db::element;
using db::property;

element :: element(const std::string &_handle, const vc::vclock &vclk)
    : handle(_handle)
    , creat_time(vclk)
    , del_time(nullptr)
    , time_oracle(nullptr)
{ }

bool
element :: add_property(const property &prop)
{
#ifdef weaver_large_property_maps_

    auto find_iter = properties.find(prop.key);
    if (find_iter == properties.end()) {
        std::vector<std::shared_ptr<property>> new_vec;
        new_vec.emplace_back(std::make_shared<property>(prop));
        properties.emplace(prop.key, new_vec);
        return true;
    } else {
        bool exists = false;
        for (const std::shared_ptr<property> p: find_iter->second) {
            if (*p == prop && !p->is_deleted()) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            find_iter->second.emplace_back(std::make_shared<property>(prop));
            return true;
        } else {
            return false;
        }
    }

#else

    bool exists = false;
    for (const std::shared_ptr<property> p: properties) {
        if (*p == prop && !p->is_deleted()) {
            exists = true;
            break;
        }
    }

    if (exists) {
        return false;
    } else {
        properties.emplace_back(std::make_shared<property>(prop));
        return true;
    }

#endif
}

bool
element :: add_property(const std::string &key, const std::string &value, const vc::vclock &vclk)
{
    property prop(key, value, vclk);
    return add_property(prop);
}

bool
element :: delete_property(const std::string &key, const vc::vclock &tdel)
{
#ifdef weaver_large_property_maps_

    auto iter = properties.find(key);
    if (iter == properties.end()) {
        return false;
    } else {
        for (std::shared_ptr<property> p: iter->second) {
            if (!p->is_deleted()) {
                p->update_del_time(tdel);
            }
        }
        return true;
    }

#else

    bool found = false;
    for (std::shared_ptr<property> p: properties) {
        if (p->key == key && !p->is_deleted()) {
            p->update_del_time(tdel);
            found = true;
        }
    }
    return found;

#endif
}

bool
element :: delete_property(const std::string &key, const std::string &value, const vc::vclock &tdel)
{
#ifdef weaver_large_property_maps_

    auto iter = properties.find(key);
    if (iter == properties.end()) {
        return false;
    } else {
        for (std::shared_ptr<property> p: iter->second) {
            if (p->key == key && p->value == value && !p->is_deleted()) {
                p->update_del_time(tdel);
                return true;
            }
        }
        return false;
    }

#else

    for (std::shared_ptr<property> p: properties) {
        if (p->key == key && p->value == value && !p->is_deleted()) {
            p->update_del_time(tdel);
            return true;
        }
    }
    return false;

#endif
}

// caution: assuming mutex access to this element
void
element :: remove_property(const std::string &key)
{
#ifdef weaver_large_property_maps_
    properties.erase(key);
#else

    std::vector<size_t> del_pos;
    size_t pos = 0;

    for (const std::shared_ptr<property> p: properties) {
        if (p->key == key) {
            del_pos.emplace_back(pos);
        }
        pos++;
    }

    for (size_t dp: del_pos) {
        properties.erase(properties.begin() + dp);
    }

#endif
}

bool
element :: has_property(const std::string &key, const std::string &value)
{
#ifdef weaver_large_property_maps_

    auto iter = properties.find(key);
    if (iter != properties.end()) {
        for (const std::shared_ptr<property> p: iter->second) {
            if (p->value == value
             && time_oracle->clock_creat_before_del_after(*view_time, p->get_creat_time(), p->get_del_time())) {
                return true;
            }
        }
    }

    return false;

#else

    for (const std::shared_ptr<property> p: properties) {
        if (p->key == key && p->value == value
         && time_oracle->clock_creat_before_del_after(*view_time, p->get_creat_time(), p->get_del_time())) {
            return true;
        }
    }
    return false;

#endif
}

bool
element :: has_property(const std::pair<std::string, std::string> &p)
{
    return has_property(p.first, p.second);
}

bool
element :: has_all_properties(const std::vector<std::pair<std::string, std::string>> &props)
{
    for (const auto &p : props) {
        if (!has_property(p)) { 
            return false;
        }
    }
    return true;
}

void
element :: update_del_time(const vc::vclock &tdel)
{
    assert(!del_time);
    del_time.reset(new vc::vclock(tdel));
}

const std::unique_ptr<vc::vclock>&
element :: get_del_time() const
{
    return del_time;
}

void
element :: update_creat_time(const vc::vclock &tcreat)
{
    creat_time = tcreat;
}

const vc::vclock&
element :: get_creat_time() const
{
    return creat_time;
}

void
element :: set_handle(const std::string &_handle)
{
    handle = _handle;
}

const std::string&
element :: get_handle() const
{
    return handle;
}
