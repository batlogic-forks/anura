/*
	Copyright (C) 2003-2013 by David White <davewx7@gmail.com>
	
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "iphone_controls.hpp"
#include "graphical_font_label.hpp"
#include "i18n.hpp"
#include "key_button.hpp"
#include "raster.hpp"
#include "surface_cache.hpp"
#include "framed_gui_element.hpp"
#include "widget_factory.hpp"

namespace gui {


	
namespace {

int vpadding = 4;
int hpadding = 10;

void stoupper(std::string& s)
{
	std::string::iterator i = s.begin();
	std::string::iterator end = s.end();

	while (i != end) {
		*i = ::toupper((unsigned char)*i);
		++i;
	}
}

}

std::string get_key_name(key_type key) 
{
	switch(key) {
	case SDLK_LEFT:
		return std::string(("←"));
	case SDLK_RIGHT:
		return std::string(("→"));
	case SDLK_UP:
		return std::string(("↑"));
	case SDLK_DOWN:
		return std::string(("↓"));
	default:
		break;
	}
	return SDL_GetKeyName(key);
}

key_type get_key_sym(const std::string& s)
{
	if(s == "UP" || s == (("↑"))) {
		return SDLK_UP;
	} else if(s == "DOWN" || s == (("↓"))) {
		return SDLK_DOWN;
	} else if(s == "LEFT" || s == (("←"))) {
		return SDLK_LEFT;
	} else if(s == "RIGHT" || s == (("→"))) {
		return SDLK_RIGHT;
	}

	return SDL_GetKeyFromName(s.c_str());
}

key_button::key_button(key_type key, buttonResolution buttonResolution)
  : label_(WidgetPtr(new graphical_font_label(get_key_name(key), "door_label", 2))),
	key_(key), button_resolution_(buttonResolution),
	normal_button_image_set_(FramedGuiElement::get("regular_button")),
	depressed_button_image_set_(FramedGuiElement::get("regular_button_pressed")),
	focus_button_image_set_(FramedGuiElement::get("regular_button_focus")),
	current_button_image_set_(normal_button_image_set_), grab_keys_(false)
	
{
	setEnvironment();
	setDim(label_->width()+hpadding*2,label_->height()+vpadding*2);
}

key_button::key_button(const variant& v, game_logic::FormulaCallable* e) 
	: widget(v,e), 	normal_button_image_set_(FramedGuiElement::get("regular_button")),
	depressed_button_image_set_(FramedGuiElement::get("regular_button_pressed")),
	focus_button_image_set_(FramedGuiElement::get("regular_button_focus")),
	current_button_image_set_(normal_button_image_set_), grab_keys_(false)
{
	std::string key = v["key"].as_string();
	key_ = get_key_sym(key);
	label_ = v.has_key("label") ? widget_factory::create(v["label"], e) : WidgetPtr(new graphical_font_label(key, "door_label", 2));
	button_resolution_ = v["resolution"].as_string_default("normal") == "normal" ? BUTTON_SIZE_NORMAL_RESOLUTION : BUTTON_SIZE_DOUBLE_RESOLUTION;

	setDim(label_->width()+hpadding*2,label_->height()+vpadding*2);
}

bool key_button::in_button(int xloc, int yloc) const
{
	translate_mouse_coords(&xloc, &yloc);
	return xloc > x() && xloc < x() + width() &&
	       yloc > y() && yloc < y() + height();
}

void key_button::handleDraw() const
{
	label_->setLoc(x()+width()/2 - label_->width()/2,y()+height()/2 - label_->height()/2);
	current_button_image_set_->blit(x(),y(),width(),height(), button_resolution_);
	label_->draw();
}

bool key_button::handleEvent(const SDL_Event& event, bool claimed)
{
    if(claimed) {
		current_button_image_set_ = normal_button_image_set_;
    }

	if(event.type == SDL_MOUSEMOTION && !grab_keys_) {
		const SDL_MouseMotionEvent& e = event.motion;
		if(current_button_image_set_ == depressed_button_image_set_) {
			//pass
		} else if(in_button(e.x,e.y)) {
			current_button_image_set_ = focus_button_image_set_;
		} else {
			current_button_image_set_ = normal_button_image_set_;
		}
	} else if(event.type == SDL_MOUSEBUTTONDOWN) {
		const SDL_MouseButtonEvent& e = event.button;
		if(in_button(e.x,e.y)) {
			current_button_image_set_ = depressed_button_image_set_;
		}
	} else if(event.type == SDL_MOUSEBUTTONUP) {
		const SDL_MouseButtonEvent& e = event.button;
		if(current_button_image_set_ == depressed_button_image_set_) {
			if(in_button(e.x,e.y)) {
				current_button_image_set_ = focus_button_image_set_;
				grab_keys_ = true;
				dynamic_cast<graphical_font_label*>(label_.get())->setText("...");
				claimed = claimMouseEvents();
			} else {
				current_button_image_set_ = normal_button_image_set_;
			}
		} else if (grab_keys_) {
			dynamic_cast<graphical_font_label*>(label_.get())->setText(get_key_name(key_));
			current_button_image_set_ = normal_button_image_set_;
			grab_keys_ = false;
		}
	}

	if(event.type == SDL_KEYDOWN && grab_keys_) {
		key_ = event.key.keysym.sym;
		if(key_ != SDLK_RETURN && key_ != SDLK_ESCAPE) {
			dynamic_cast<graphical_font_label*>(label_.get())->setText(get_key_name(key_));
			claimed = true;
			current_button_image_set_ = normal_button_image_set_;
			grab_keys_ = false;
		}
	}

	return claimed;
}

key_type key_button::get_key() {
	return key_;
}

void key_button::setValue(const std::string& key, const variant& v)
{
	widget::setValue(key, v);
}

variant key_button::getValue(const std::string& key) const
{
	if(key == "key") {
		return variant(key_);
	}
	return widget::getValue(key);
}

}
