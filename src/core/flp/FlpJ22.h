// Gridline — FLP import, J-22 (instrument parity tier C): decoders that map FL
// synths onto dedicated Gridline engines (or reference models of existing ones).
// Registered after every earlier wave and REPLACING their approximate decoders.
#pragma once
#include <map>
#include <string>
#include "Flp.h"

namespace gl::flp::j22 {

// Registers (overriding any earlier decoder of the same FL name).
inline void set(std::map<std::string, PluginDecoder>& d, const std::string& name, PluginDecoder dec) { d[name] = std::move(dec); }

void registerDrums(std::map<std::string, PluginDecoder>& d);    // FlpJ22Drums.cpp
void registerStrings(std::map<std::string, PluginDecoder>& d);  // FlpJ22Strings.cpp
void registerMetal(std::map<std::string, PluginDecoder>& d);    // FlpJ22Metal.cpp
void registerVa(std::map<std::string, PluginDecoder>& d);       // FlpJ22Va.cpp
void registerDco(std::map<std::string, PluginDecoder>& d);      // FlpJ22Dco.cpp

}  // namespace gl::flp::j22

namespace gl::flp {
void registerDecodersJ22(std::map<std::string, PluginDecoder>& d);  // FlpJ22.cpp
}
