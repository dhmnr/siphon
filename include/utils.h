#pragma once

#include "process_attribute.h"
#include <map>
#include <string>

bool IsRunAsAdmin();
bool GetProcessInfoFromTOML(const std::string &filepath, std::string *processName,
                            std::map<std::string, ProcessAttribute> *attributes,
                            std::string *processWindowName);
void PrintProcessAttributes(const std::map<std::string, ProcessAttribute> &attributes);