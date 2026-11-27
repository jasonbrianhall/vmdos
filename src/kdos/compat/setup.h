/* kdos: DOSBox-X's configuration, reduced to what its DOS and shell read.
   Values come from a table in glue.cpp (kdos_cfg), not a dosbox.conf. */
#pragma once
#include "dosbox.h"
class Hex {
    int _hex;
public:
    Hex(int in) : _hex(in) {}
    Hex() : _hex(0) {}
    bool operator==(Hex const &other) const { return _hex == other._hex; }
    operator int() const { return _hex; }
};
class Property {
public:
    std::string name, value;
};
class Section {
public:
    std::string sectionname;
    std::string data;
    virtual ~Section() {}
    const char *GetName() const { return sectionname.c_str(); }
    virtual std::string GetPropValue(std::string const &p) const = 0;
    bool HandleInputline(std::string const &) { return true; }
};
class Section_prop : public Section {
public:
    Section_prop(const char *n) { sectionname = n; }
    int Get_int(std::string const &p) const;
    const char *Get_string(std::string const &p) const;
    bool Get_bool(std::string const &p) const;
    Hex Get_hex(std::string const &p) const;
    double Get_double(std::string const &p) const;
    Property *Get_prop(int) { return nullptr; }
    std::string GetPropValue(std::string const &p) const override;
};
class Section_line : public Section {
public:
    Section_line(const char *n) { sectionname = n; }
    std::string GetPropValue(std::string const &) const override { return "NO_SUCH_PROPERTY"; }
};
