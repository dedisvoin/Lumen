#include <fstream>
#include <sstream>
#include "C:\Users\Ivan\Desktop\Lumen\compiler\runtime\lumen_rt.h"

rt::Box lf_read_file(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();

    rt::Box arg = rt::derefval(args_[0]);
    arg = rt::deref(arg);
    if (arg.t != rt::Box::T_STR) return rt::Box::null();

    std::ifstream file(arg.s, std::ios::binary);
    if (!file.is_open()) return rt::Box::null();

    std::ostringstream buf;
    buf << file.rdbuf();
    return rt::Box::str(buf.str());
}

rt::Box lf_write_file(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::boo(false);

    rt::Box name_arg = rt::deref(rt::derefval(args_[0]));
    rt::Box data_arg = rt::deref(rt::derefval(args_[1]));
    if (name_arg.t != rt::Box::T_STR || data_arg.t != rt::Box::T_STR)
        return rt::Box::boo(false); // оба аргумента должны быть строками

    std::ofstream file(name_arg.s, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return rt::Box::boo(false); // нет доступа / некорректный путь

    file.write(data_arg.s.data(), (std::streamsize)data_arg.s.size());
    return rt::Box::boo(file.good());
}

rt::Box lf_file_exists(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::boo(false);

    rt::Box name_arg = rt::deref(rt::derefval(args_[0]));
    if (name_arg.t != rt::Box::T_STR) return rt::Box::boo(false);

    std::ifstream file(name_arg.s);
    return rt::Box::boo(file.good());
}

rt::Box lf_append_file(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.size() < 2) return rt::Box::boo(false);

    rt::Box name_arg = rt::deref(rt::derefval(args_[0]));
    rt::Box data_arg = rt::deref(rt::derefval(args_[1]));
    if (name_arg.t != rt::Box::T_STR || data_arg.t != rt::Box::T_STR)
        return rt::Box::boo(false);

    std::ofstream file(name_arg.s, std::ios::binary | std::ios::app);
    if (!file.is_open()) return rt::Box::boo(false);

    file.write(data_arg.s.data(), (std::streamsize)data_arg.s.size());
    return rt::Box::boo(file.good());
}

rt::Box lf_delete_file(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::boo(false);

    rt::Box name_arg = rt::deref(rt::derefval(args_[0]));
    if (name_arg.t != rt::Box::T_STR) return rt::Box::boo(false);

    return rt::Box::boo(std::remove(name_arg.s.c_str()) == 0); // <cstdio>
}

rt::Box lf_file_size(rt::Ctx ctx, const std::vector<rt::Box>& args_) {
    if (args_.empty()) return rt::Box::null();

    rt::Box name_arg = rt::deref(rt::derefval(args_[0]));
    if (name_arg.t != rt::Box::T_STR) return rt::Box::null();

    std::ifstream file(name_arg.s, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return rt::Box::null();

    return rt::Box::i64((long long)file.tellg()); // ate -> позиция в конце = размер
}