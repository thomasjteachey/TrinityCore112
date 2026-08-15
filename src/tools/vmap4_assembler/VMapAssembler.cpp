/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <iostream>

#include "TileAssembler.h"
#include "Banner.h"
#include "Locales.h"
#include "Util.h"

// Parses "1608" or "1608,1620,617" into ids. Returns false on anything else.
static bool ParseMapIdList(char const* text, std::set<uint32>& ids)
{
    if (!text || !*text)
        return false;

    std::string token;
    for (char const* p = text; ; ++p)
    {
        if (*p == ',' || *p == '\0')
        {
            if (token.empty())
                return false;
            for (char ch : token)
                if (!isdigit(static_cast<unsigned char>(ch)))
                    return false;
            ids.insert(static_cast<uint32>(strtoul(token.c_str(), nullptr, 10)));
            token.clear();
            if (*p == '\0')
                break;
        }
        else
            token.push_back(*p);
    }
    return !ids.empty();
}

static void Usage(char const* prg)
{
    std::cout << "usage: " << prg << " [<raw data dir>] [<vmap dest dir>] [-m <map ids>]" << std::endl;
    std::cout << "   <raw data dir>  : vmap4extractor output (default: Buildings)" << std::endl;
    std::cout << "   <vmap dest dir> : where .vmtree/.vmtile/.vmo files are written (default: vmaps)" << std::endl;
    std::cout << "   -m <map ids>    : only assemble these maps, one id or a comma-separated list" << std::endl;
    std::cout << "                     (e.g. -m 1608 or -m 1608,1620). Only the models those maps spawn" << std::endl;
    std::cout << "                     are converted and the gameobject model list is left alone." << std::endl;
}

int main(int argc, char* argv[])
{
    Trinity::VerifyOsVersion();

    Trinity::Locale::Init();

    Trinity::Banner::Show("VMAP assembler", [](char const* text) { std::cout << text << std::endl; }, nullptr);

    std::string src = "Buildings";
    std::string dest = "vmaps";
    std::set<uint32> mapFilter;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-m") == 0)
        {
            if (i + 1 < argc && ParseMapIdList(argv[i + 1], mapFilter))
                ++i;
            else
            {
                Usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "--help") == 0)
        {
            Usage(argv[0]);
            return 1;
        }
        else
            positional.push_back(argv[i]);
    }

    if (positional.size() > 2)
    {
        Usage(argv[0]);
        return 1;
    }
    if (positional.size() > 0)
        src = positional[0];
    if (positional.size() > 1)
        dest = positional[1];

    std::cout << "using " << src << " as source directory and writing output to " << dest << std::endl;

    VMAP::TileAssembler* ta = new VMAP::TileAssembler(src, dest);
    if (!mapFilter.empty())
        ta->setMapFilter(mapFilter);

    if (!ta->convertWorld2())
    {
        std::cout << "exit with errors" << std::endl;
        delete ta;
        return 1;
    }

    delete ta;
    std::cout << "Ok, all done" << std::endl;
    return 0;
}
