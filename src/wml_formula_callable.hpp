/*
	Copyright (C) 2003-2014 by David White <davewx7@gmail.com>
	
	This software is provided 'as-is', without any express or implied
	warranty. In no event will the authors be held liable for any damages
	arising from the use of this software.

	Permission is granted to anyone to use this software for any purpose,
	including commercial applications, and to alter it and redistribute it
	freely, subject to the following restrictions:

	   1. The origin of this software must not be misrepresented; you must not
	   claim that you wrote the original software. If you use this software
	   in a product, an acknowledgement in the product documentation would be
	   appreciated but is not required.

	   2. Altered source versions must be plainly marked as such, and must not be
	   misrepresented as being the original software.

	   3. This notice may not be removed or altered from any source
	   distribution.
*/

#pragma once

#include <map>
#include <string>

#include <stdint.h>

#include <functional>

#include <boost/intrusive_ptr.hpp>

#include "formula_callable.hpp"
#include "variant.hpp"

namespace game_logic
{
	// To make a FormulaCallable that is serializable, follow these steps:
	// - Derive from WmlSerializableFormulaCallable instead of FormulaCallable
	// - Implement a constructor which takes a variant as its only argument and
	//   deserializes the object. In this constructor, put
	//   READ_SERIALIZABLE_CALLABLE(node) where node is the passed in variant.
	// - Implement the serializeToWml() method which should return a variant
	//   which is a map. Choose a unique string key beginning with @ that is
	//   used for instances of this class and return it as part of the map.
	//   For instance if your class is a foo_callable you might want to make the
	//   string "@foo".
	// - In your cpp file add REGISTER_SERIALIZABLE_CALLABLE(foo_callable, "@foo").
	//   This will do the magic to ensure that the FSON post-processor will
	//   deserialize your object when an instance is found.

	class WmlSerializableFormulaCallable : public FormulaCallable
	{
	public:
		static int registerSerializableType(const char* name, std::function<variant(variant)> ctor);
		static const std::map<std::string, std::function<variant(variant)> >& registeredTypes();
		static bool deserializeObj(const variant& var, variant* target);

		explicit WmlSerializableFormulaCallable(bool has_self=true) : FormulaCallable(has_self) {}

		virtual ~WmlSerializableFormulaCallable() {}

		variant writeToWml() const;

		const std::string& addr() const { return addr_; }
	protected:
		void setAddr(const std::string& addr) { addr_ = addr; }
	private:
		virtual variant serializeToWml() const = 0;
		std::string addr_;
	};

	#define REGISTER_SERIALIZABLE_CALLABLE(classname, idname) \
		static const int classname##_registration_var_unique__ = game_logic::WmlSerializableFormulaCallable::registerSerializableType(idname, [](variant v) ->variant { return variant(new classname(v)); });

	#define READ_SERIALIZABLE_CALLABLE(node) if(node.has_key("_addr")) { setAddr(node["_addr"].as_string()); }

	typedef boost::intrusive_ptr<WmlSerializableFormulaCallable> WmlSerializableFormulaCallablePtr;
	typedef boost::intrusive_ptr<const WmlSerializableFormulaCallable> ConstWmlSerializableFormulaCallablePtr;

	class wmlFormulaCallableSerializationScope
	{
	public:
		static void registerSerializedObject(ConstWmlSerializableFormulaCallablePtr ptr);
		static bool isActive();

		wmlFormulaCallableSerializationScope();
		~wmlFormulaCallableSerializationScope();

		variant writeObjects(variant obj, int* num_objects=0) const;

	private:
	};

	class wmlFormulaCallableReadScope
	{
	public:
		static void registerSerializedObject(intptr_t addr, WmlSerializableFormulaCallablePtr ptr);
		static WmlSerializableFormulaCallablePtr getSerializedObject(intptr_t addr);
		wmlFormulaCallableReadScope();
		~wmlFormulaCallableReadScope();

		static bool try_load_object(intptr_t id, variant& v);
	private:
	};

	std::string serialize_doc_with_objects(variant v);
	variant deserialize_doc_with_objects(const std::string& msg);
	variant deserialize_file_with_objects(const std::string& fname);

}
