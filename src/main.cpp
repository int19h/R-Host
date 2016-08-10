/* ****************************************************************************
 *
 * Copyright (c) Microsoft Corporation. All rights reserved. 
 *
 *
 * This file is part of Microsoft R Host.
 * 
 * Microsoft R Host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Microsoft R Host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Microsoft R Host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ***************************************************************************/

#include "stdafx.h"
#include "msvcrt.h"
#include "log.h"
#include "r_util.h"
#include "host.h"
#include "Rapi.h"
#include "util.h"
#include "eval.h"
#include "detours.h"
#include "grdeviceside.h"
#include "grdevicesxaml.h"
#include "exports.h"
#include "transport.h"

using namespace rhost::eval;
using namespace rhost::log;
using namespace rhost::util;
namespace po = boost::program_options;

namespace rhost {
    struct command_line_args {
        std::string name;
        boost::optional<boost::asio::ip::tcp::endpoint> listen_endpoint;
        boost::optional<websocketpp::uri> connect_uri;

        std::vector<std::string> unrecognized;
        int argc;
        std::vector<char*> argv;
    };

    command_line_args parse_command_line(int argc, char** argv) {
        command_line_args args;

        po::option_description
            help("rhost-help", new po::untyped_value(true), "Produce help message."),
            name("rhost-name", po::value<std::string>(), "Name of this host instance.");

        po::options_description desc;
        for (auto&& opt : { help, name }) {
            boost::shared_ptr<po::option_description> popt(new po::option_description(opt));
            desc.add(popt);
        }

        po::variables_map vm;
        try {
            auto parsed = po::command_line_parser(argc, argv).options(desc).allow_unregistered().run();
            po::store(parsed, vm);
            po::notify(vm);
            args.unrecognized = po::collect_unrecognized(parsed.options, po::include_positional);
        } catch (po::error& e) {
            std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
            std::cerr << desc << std::endl;
            std::exit(EXIT_FAILURE);
        }

        if (vm.count(help.long_name())) {
            std::cerr << desc << std::endl;
            std::exit(EXIT_SUCCESS);
        }

        auto name_arg = vm.find(name.long_name());
        if (name_arg != vm.end()) {
            args.name = name_arg->second.as<std::string>();
        }

        args.argv.push_back(argv[0]);
        for (auto& s : args.unrecognized) {
            args.argv.push_back(&s[0]);
        }
        args.argc = int(args.argv.size());
        args.argv.push_back(nullptr);

        return args;
    }

    void set_memory_limit() {
#ifdef WIN32
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof ms;
        if (!GlobalMemoryStatusEx(&ms)) {
            logf("Couldn't set R memory limit - GlobalMemoryStatusEx failed with GetLastError=%u\n", GetLastError());
            return;
        }

        double memory_limit = static_cast<double>(ms.ullTotalPhys / 1024 / 1024);
        logf("Setting R memory limit to %0.0f MB\n", memory_limit);

        protected_sexp limit = Rf_allocVector(REALSXP, 1);
        *REAL(limit.get()) = memory_limit;
        if (!r_top_level_exec([&] { in_memsize(limit.get()); })) {
            logf("Couldn't set R memory limit - in_memsize failed: %s\n", R_curErrorBuf());
        }
#endif
    }

    void suggest_mro(structRstart& rp) {
        ParseStatus ps;
        auto res = r_try_eval_str("if (exists('Revo.version')) 'REVO' else 'CRAN'", R_BaseEnv, ps);

        assert(res.has_value);
        if (!res.has_value) {
            return;
        }

        if (res.value == "REVO") {
            // This is Revolution R or Microsoft R.
            return;
        }

        static const char mro_banner[] = "Check out Microsoft's enhanced R distribution at http://go.microsoft.com/fwlink/?LinkId=734720. \n\n";
        rp.WriteConsoleEx(mro_banner, static_cast<int>(strlen(mro_banner)), 0);
    }

    int run(command_line_args& args) {
        init_log(args.name);
        transport::initialize();

        R_setStartTime();
        structRstart rp = {};
        R_DefParams(&rp);

        rp.rhome = get_R_HOME();
        rp.home = getRUser();
        rp.CharacterMode = RGui;
        rp.R_Quiet = R_FALSE;
        rp.R_Interactive = R_TRUE;
        rp.RestoreAction = SA_RESTORE;
        rp.SaveAction = SA_NOSAVE;

        rhost::host::initialize(rp);
        rhost::detours::init_ui_detours();

        R_set_command_line_arguments(args.argc, args.argv.data());
        R_common_command_line(&args.argc, args.argv.data(), &rp);
        R_SetParams(&rp);

        GA_initapp(0, 0);
        readconsolecfg();

        DllInfo *dll = R_getEmbeddingDllInfo();
        rhost::r_util::init(dll);
        rhost::grdevices::xaml::init(dll);
        rhost::grdevices::ide::init(dll);
        rhost::exports::register_all(dll);

        CharacterMode = LinkDLL;
        setup_Rmainloop();
        CharacterMode = RGui;

        set_memory_limit();

        // setup_Rmainloop above prints out the license banner, so this will follow that.
        suggest_mro(rp);

        run_Rmainloop();

        Rf_endEmbeddedR(0);
        return EXIT_SUCCESS;
    }

    int run(int argc, char** argv) {
        return rhost::run(rhost::parse_command_line(argc, argv));
    }
}

int main(int argc, char** argv) {
    //MessageBox(0, 0, 0, 0);
    setlocale(LC_NUMERIC, "C");
    __try {
        return rhost::run(argc, argv);
    } __finally {
        flush_log();
        rhost::detours::terminate_ui_detours();
    }
}
