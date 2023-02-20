/*
 * Copyright (c) 2016, 2017, Yutaka Tsutano
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "axmldec_config.hpp"
#include "jitana/util/axml_parser.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/program_options.hpp>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
#define PLATFORM_WINDOWS
#include <io.h>
#include <fcntl.h>
#endif

#include <unzip.h>

#define AXMLDEC_VERSION AXMLDEC_VERSION_MAJOR "." AXMLDEC_VERSION_MINOR "." AXMLDEC_VERSION_PATCH

namespace boost_pt = boost::property_tree;

struct membuf : std::streambuf {
    membuf(const char* base, size_t size)
    {
        char* p(const_cast<char*>(base));
        this->setg(p, p, p + size);
    }
};

struct imemstream : virtual membuf, std::istream {
    imemstream(char const* base, size_t size)
            : membuf(base, size),
              std::istream(static_cast<std::streambuf*>(this))
    {
    }
};

std::vector<char> extract_file(const std::string& path, const std::string& entry)
{
    auto* apk = unzOpen(path.c_str());
    if (apk == nullptr) {
        throw std::runtime_error("not an APK file");
    }

    if (unzLocateFile(apk, entry.c_str(), 0) != UNZ_OK) {
        unzClose(apk);
        throw std::runtime_error(entry+" is not found in APK");
    }

    if (unzOpenCurrentFile(apk) != UNZ_OK) {
        unzClose(apk);
        throw std::runtime_error("failed to open "+entry+" in APK");
    }

    unz_file_info64 info;
    if (unzGetCurrentFileInfo64(apk, &info, nullptr, 0, nullptr, 0, nullptr, 0)
        != UNZ_OK) {
        unzCloseCurrentFile(apk);
        unzClose(apk);
        throw std::runtime_error("failed to open "+entry+" in APK");
    }

    std::vector<char> content(info.uncompressed_size);
    constexpr size_t read_size = 1 << 15;
    for (size_t offset = 0;; offset += read_size) {
        int len = unzReadCurrentFile(apk, content.data() + offset, read_size);
        if (len == 0) {
            break;
        }
        else if (len < 0) {
            unzCloseCurrentFile(apk);
            unzClose(apk);
            throw std::runtime_error("failed to read file "+entry+" in APK");
        }
    }

    unzCloseCurrentFile(apk);
    unzClose(apk);

    return content;
}

void write_xml(const std::string& path, const boost_pt::ptree& pt)
{
    // Construct the output stream.
    std::ostream* os = &std::cout;
    std::ofstream ofs;
    if (!path.empty()) {
        ofs.open(path);
        os = &ofs;
    }

    // Write the ptree to the output.
    {
#if BOOST_MAJOR_VERSION == 1 && BOOST_MINOR_VERSION < 56
        boost_pt::xml_writer_settings<char> settings(' ', 2);
#else
        boost_pt::xml_writer_settings<std::string> settings(' ', 2);
#endif
        boost_pt::write_xml(*os, pt, settings);
    }
}

void process_file(const std::string& inpath,
                  const std::string& entry,
                  const std::string& outpath)
{
    // Property tree for storing the XML content.
    boost_pt::ptree pt;

    // Load the XML into ptree.
    std::istream* is = &std::cin;
    std::ifstream ifs;
    if (!inpath.empty() && inpath != "-") {
        ifs.open(inpath, std::ios::binary);
        is = &ifs;
    }
    else {
        //TODO Linux?
        #ifdef PLATFORM_WINDOWS
        _setmode(_fileno(stdin), _O_BINARY);
        #endif
    }
    if (is->peek() == 'P') {
        auto content = extract_file(inpath, entry);
        imemstream ims(content.data(), content.size());
        jitana::read_axml(ims, pt);
    }
    else if (is->peek() == 0x03) {
        jitana::read_axml(*is, pt);
    }
    else {
        boost_pt::read_xml(*is, pt, boost_pt::xml_parser::trim_whitespace);
    }

    // Write the tree as an XML file.
    write_xml(outpath, pt);
}

int main(int argc, char** argv)
{
    namespace po = boost::program_options;

    // Declare command line argument options.
    po::options_description desc(
        "axmldec " AXMLDEC_VERSION " (" AXMLDEC_BUILD_TIMESTAMP ") Copyright (C) 2017 Yutaka Tsutano.\n\n"
        "Usage: axmldec [-h] [-v] [INPUT] [ENTRY] [-o OUTPUT]\n"
        "Decodes an AXML file, optionally inside an APK file.\n\nOptions");
    desc.add_options()
        ("input,i", po::value<std::string>(), "Path to the input file.\nDefault: standard input")
        ("entry,e", po::value<std::string>(), "Entry name in the input file.\nDefault: AndroidManifest.xml")
        ("output,o", po::value<std::string>(), "Path to the output file.\nDefault: standard output")
        ("version,v", "Print version number.")
        ("help,h", "Show this help message and exit.");
    po::positional_options_description p;
    p.add("input", 1);
    p.add("entry", 1);

    try {
        // Process the command line arguments.
        po::variables_map vmap;
        po::store(po::command_line_parser(argc, argv)
                          .options(desc)
                          .positional(p)
                          .run(),
                  vmap);
        po::notify(vmap);

        if (vmap.count("help")) {
            // Print help and quit.
            std::cout << desc;
            return 0;
        }

        if (vmap.count("version")) {
            // Print version and quit.
            std::cout << AXMLDEC_VERSION << '\n';
            return 0;
        }

        auto inpath = vmap.count("input")
                ? vmap["input"].as<std::string>()
                : "";
        auto entry = vmap.count("entry")
                ? vmap["entry"].as<std::string>()
                : "AndroidManifest.xml";
        auto outpath = vmap.count("output")
                ? vmap["output"].as<std::string>()
                : "";

        // Process the file.
        process_file(inpath, entry, outpath);
    }
    catch (std::ios::failure& e) {
        std::cerr << "error: failed to open the input file\n";
        return 1;
    }
    catch (std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
