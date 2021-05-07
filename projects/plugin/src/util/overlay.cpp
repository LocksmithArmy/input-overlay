/*************************************************************************
 * This file is part of input-overlay
 * github.con/univrsal/input-overlay
 * Copyright 2021 univrsal <uni@vrsal.de>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "overlay.hpp"
#include "../sources/input_source.hpp"
#include "config.hpp"
#include "element/element.hpp"
#include "element/element_analog_stick.hpp"
#include "element/element_button.hpp"
#include "element/element_dpad.hpp"
#include "element/element_gamepad_id.hpp"
#include "element/element_mouse_movement.hpp"
#include "element/element_mouse_wheel.hpp"
#include "element/element_trigger.hpp"
#include "../gui/io_settings_dialog.hpp"
#include "../hook/gamepad_hook_helper.hpp"
#include "log.h"
#include "../network/io_server.hpp"
#include "../network/remote_connection.hpp"
#include "obs_util.hpp"
#include "lang.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <layout_constants.h>
extern "C" {
#include <graphics/image-file.h>
}

namespace sources {
class overlay_settings;
}

overlay::~overlay()
{
    unload();
}

overlay::overlay(sources::overlay_settings *settings)
{
    m_settings = settings;
    m_is_loaded = load();
}

bool overlay::load()
{
    unload();
    const auto image_loaded = load_texture();
    m_is_loaded = image_loaded && load_cfg();

    if (!m_is_loaded) {
        m_settings->gamepad = 0;
        if (!image_loaded) {
            m_settings->cx = 100; /* Default size */
            m_settings->cy = 100;
        }
    }

    return m_is_loaded;
}

void overlay::unload()
{
    unload_texture();
    unload_elements();
    m_settings->cx = 100;
    m_settings->cy = 100;
}

bool overlay::load_cfg()
{
    if (!m_settings || m_settings->layout_file.empty())
        return false;

    QFile file(m_settings->layout_file.c_str());

    if (!file.open(QIODevice::ReadOnly)) {
        blog(LOG_ERROR, "[input-overlay] couldn't open config file");
        return false;
    }

    QJsonParseError err;
    const auto cfg_doc = QJsonDocument::fromJson(file.readAll(), &err);
    auto cfg_obj = cfg_doc.object();
    const auto flag = true;

    if (err.error == QJsonParseError::NoError) {
        m_settings->cx = static_cast<uint32_t>(cfg_obj[CFG_TOTAL_WIDTH].toInt());
        m_settings->cy = static_cast<uint32_t>(cfg_obj[CFG_TOTAL_HEIGHT].toInt());
        m_settings->layout_flags = static_cast<uint8_t>(cfg_obj[CFG_FLAGS].toInt());

        const auto debug_mode = cfg_obj[CFG_DEBUG_FLAG].toBool();

#ifndef _DEBUG
        if (debug_mode) {
#else
        {
#endif
            binfo("Started loading of %s", m_settings->layout_file.c_str());
        }

        auto arr = cfg_obj[CFG_ELEMENTS].toArray();

        for (const auto element : arr)
            load_element(element.toObject(), debug_mode);
    } else {
        berr("Couldn't load layout from %s. Error: %s", m_settings->layout_file.c_str(), qt_to_utf8(err.errorString()));
    }

    return flag;
}

bool overlay::load_texture()
{
    if (!m_settings || m_settings->image_file.empty())
        return false;

    auto flag = true;

    if (m_image == nullptr) {
        m_image = new gs_image_file_t();
    }

    gs_image_file_init(m_image, m_settings->image_file.c_str());

    obs_enter_graphics();
    gs_image_file_init_texture(m_image);
    obs_leave_graphics();

    if (!m_image->loaded) {
        bwarn("Error: failed to load texture %s", m_settings->image_file.c_str());
        flag = false;
    } else {
        m_settings->cx = m_image->cx;
        m_settings->cy = m_image->cy;
    }

    return flag;
}

void overlay::unload_texture() const
{
    obs_enter_graphics();
    gs_image_file_free(m_image);
    obs_leave_graphics();
}

void overlay::unload_elements()
{
    m_elements.clear();
}

void overlay::draw(gs_effect_t *effect)
{
    if (m_is_loaded) {
        for (auto const &element : m_elements) {
            element->draw(effect, m_image, m_settings);
        }
    }
}

void overlay::refresh_data()
{
    /* This copies over necessary input data information
     * to make sure the overlay always has data available to
     * draw the overlay. If the data was directly accessed in the render
     * method, the overlay can start to flicker if the frame is rendered
     * while the data is currently inaccessible, because it is being written
     * to by the input thread, resulting in all buttons being unpressed
     */
    if (io_config::io_window_filters.input_blocked())
        return;
    input_data *source = nullptr;
    std::mutex *m = nullptr;
    std::shared_ptr<network::io_client> client = nullptr; // Holds the reference until we've copied the data
    if (uiohook::state || network::network_flag || libgamepad::state) {
        if (network::server_instance && !m_settings->use_local_input()) {
            client = network::server_instance->get_client(m_settings->selected_source);
            if (client && client->valid()) {
                source = client->get_data();
                m = &network::mutex;
            }
        } else {
            source = &local_data::data;
            m = &local_data::data_mutex;
        }
    }

    if (source) {
        // copy over data from gamepad into the input data structure
        auto copy = [](input_data *source, std::shared_ptr<gamepad::device> d) {
            source->last_axis_event = *d->last_axis_event();
            source->last_button_event = *d->last_button_event();
            source->gamepad_axis = d->get_axis();
            source->gamepad_buttons = d->get_buttons();
        };

        if (libgamepad::hook_instance && m_settings->use_local_input()) {
            if (m_settings->gamepad) {
                libgamepad::hook_instance->get_mutex()->lock();
                copy(source, m_settings->gamepad);
                libgamepad::hook_instance->get_mutex()->unlock();
            }
            m->lock();
            m_settings->data.copy(source);
            if (uiohook::state)
                uiohook::check_wheel();
            m->unlock();
        } else {
            m->lock();
            if (m_settings->gamepad)
                copy(source, m_settings->gamepad);
            m_settings->data.copy(source);
            m->unlock();
        }
    }
}

void overlay::load_element(const QJsonObject &obj, const bool debug)
{
    const auto type = obj[CFG_TYPE].toInt();
    element *new_element = nullptr;

    switch (type) {
    case ET_TEXTURE:
        new_element = new element_texture;
        break;
    case ET_GAMEPAD_ID:
        new_element = new element_gamepad_id;
        break;
    case ET_KEYBOARD_KEY:
        new_element = new element_keyboard_key;
        break;
    case ET_MOUSE_BUTTON:
        new_element = new element_mouse_button;
        break;
    case ET_GAMEPAD_BUTTON:
        new_element = new element_gamepad_button;
        break;
    case ET_WHEEL:
        new_element = new element_wheel;
        break;
    case ET_TRIGGER:
        new_element = new element_trigger;
        break;
    case ET_ANALOG_STICK:
        new_element = new element_analog_stick;
        break;
    case ET_DPAD_STICK:
        new_element = new element_dpad;
        break;
    case ET_MOUSE_MOVEMENT:
        new_element = new element_mouse_movement;
        break;
    default:
        if (debug)
            binfo("Invalid element type %i for %s", type, qt_to_utf8(obj[CFG_ID].toString()));
    }

    if (new_element) {
        new_element->load(obj);
        m_elements.emplace_back(new_element);

#ifndef _DEBUG
        if (debug) {
#else
        {
#endif
            binfo("Type: %14s, KEYCODE: 0x%04X ID: %s", element_type_to_string(static_cast<element_type>(type)),
                  new_element->get_keycode(), qt_to_utf8(obj[CFG_ID].toString()));
        }
    }
}

const char *overlay::element_type_to_string(const element_type t)
{
    switch (t) {
    case ET_TEXTURE:
        return "Texture";
    case ET_KEYBOARD_KEY:
        return "Keyboard key";
    case ET_MOUSE_BUTTON:
        return "Mouse button";
    case ET_GAMEPAD_BUTTON:
        return "Gamepad button";
    case ET_ANALOG_STICK:
        return "Analog stick";
    case ET_WHEEL:
        return "Scroll wheel";
    case ET_MOUSE_MOVEMENT:
        return "Mouse movement";
    case ET_TRIGGER:
        return "Trigger";
    case ET_GAMEPAD_ID:
        return "Gamepad ID";
    case ET_DPAD_STICK:
        return "DPad";
    default:
    case ET_INVALID:
        return "Invalid";
    }
}
