#!/usr/bin/env python3
"""Generate root/res/licenses/THIRD_PARTY_NOTICES.txt.

Every package copies root/res, so a file there ships with all of them. The
license texts are taken verbatim, either from the vendored sources in this
repository or from the library's repository at the revision pinned by its
deps/+<name>/<name>.cmake recipe. Re-run this script and commit the result
after bumping a dependency or vendoring a library.

Usage: scripts/generate_third_party_notices.py

Any fetch failure aborts the run, so a partial file is never written.
"""
import io
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / 'root/res/licenses/THIRD_PARTY_NOTICES.txt'
APACHE_2 = 'https://www.apache.org/licenses/LICENSE-2.0.txt'


def gh(repo, ref, *paths):
    return [f'https://raw.githubusercontent.com/{repo}/{ref}/{path}' for path in paths]


def local(*paths):
    return [('file', path) for path in paths]


def excerpt(path, start, end):
    """Lines of a vendored file from the one matching start to the one matching end, inclusive."""
    return [('excerpt', path, start, end)]


def archive(url, *members):
    """Top-level files of a zip archive, for a project with no browsable repository."""
    return [('archive', url, member) for member in members]


# (name, version, homepage, license sources)
LIBRARIES = [
    # Vendored in this repository
    ('Dear ImGui', '1.87', 'https://github.com/ocornut/imgui', gh('ocornut/imgui', 'v1.87', 'LICENSE.txt')),
    ('stb_image, stb_image_resize, stb_truetype, stb_rect_pack, stb_textedit', '', 'https://github.com/nothings/stb',
     excerpt('core/src/imgui/stb_image.h', r'^ALTERNATIVE A - MIT License', r'SOFTWARE\.$')),
    ('JSON for Modern C++', '3.9.0', 'https://github.com/nlohmann/json', gh('nlohmann/json', 'v3.9.0', 'LICENSE.MIT')),
    ('ghc::filesystem', '', 'https://github.com/gulrak/filesystem',
     excerpt('core/std_replacement/filesystem', r'Copyright \(c\) 2018, Steffen', r'SOFTWARE\.$')),
    ('Portable File Dialogs', '', 'https://github.com/samhocevar/portable-file-dialogs',
     excerpt('core/src/gui/file_dialogs.h', r'Copyright', r'See http://www\.wtfpl\.net/')),
    ('earcut.hpp', '', 'https://github.com/mapbox/earcut.hpp',
     excerpt('core/src/gui/brown/earcut.hpp', r'^ISC License', r'THIS SOFTWARE\.$')),
    ('picohash', '', 'https://github.com/kazuho/picohash',
     excerpt('core/src/utils/proto/picohash.h', r'public domain', r'liboauth')),
    ('ENet', '', 'https://github.com/zpl-c/enet',
     excerpt('source_modules/qmxserver_source/src/enet/enet.h', r'The MIT License \(MIT\)', r'SOFTWARE\.$')),
    ('libcorrect', '', 'https://github.com/quiet/libcorrect', local('core/libcorrect/LICENSE')),
    ('discord-rpc', '', 'https://github.com/discord/discord-rpc', local('misc_modules/discord_integration/discord-rpc/LICENSE')),
    ('RapidJSON', '', 'https://github.com/Tencent/rapidjson', gh('Tencent/rapidjson', 'v1.1.0', 'license.txt')),
    ('libsddc', '', 'https://github.com/ik1xpv/ExtIO_sddc', local('source_modules/sddc_source/libsddc/license')),
    ('sdrpp_radiosonde', '', 'https://github.com/dbdexter-dev/sdrpp_radiosonde', local('decoder_modules/radiosonde_decoder/LICENSE')),
    ('logmmse', '', 'https://github.com/rajivpoddar/logmmse', gh('rajivpoddar/logmmse', 'master', 'LICENSE')),
    ('Roboto font', '', 'https://github.com/googlefonts/roboto', [APACHE_2]),

    # Built from deps/
    ('codec2', '310777b', 'https://github.com/drowe67/codec2', gh('drowe67/codec2', '310777b1c6f1af0bc7c72f5b32f80f6fd9136962', 'COPYING')),
    ('FFTW', '3.3.10', 'https://www.fftw.org', gh('FFTW/fftw3', 'fftw-3.3.10', 'COPYRIGHT', 'COPYING')),
    ('FLAC', '1.5.0', 'https://github.com/xiph/flac', gh('xiph/flac', '1.5.0', 'COPYING.Xiph')),
    ('GLFW', '3.4', 'https://github.com/glfw/glfw', gh('glfw/glfw', '3.4', 'LICENSE.md')),
    ('libad9361-iio', '0.2', 'https://github.com/analogdevicesinc/libad9361-iio', gh('analogdevicesinc/libad9361-iio', 'v0.2', 'LICENSE')),
    ('libairspy', 'c672100', 'https://github.com/airspy/airspyone_host', gh('airspy/airspyone_host', 'c6721000f19601512e9ba6b0340e5d9ced22a900', 'libairspy/LICENSE.md')),
    ('libairspyhf', '30e0422', 'https://github.com/airspy/airspyhf', gh('airspy/airspyhf', '30e0422ab5eae0fecd429b29e89c6e8699bb5cd6', 'LICENSE.BSD', 'LICENSE.GPL-2.0')),
    ('libbladeRF', '2.5.0', 'https://github.com/Nuand/bladeRF', gh('Nuand/bladeRF', 'libbladeRF_v2.5.0', 'COPYING')),
    ('curl', '8.22.0', 'https://github.com/curl/curl', gh('curl/curl', 'curl-8_22_0', 'COPYING')),
    ('libfobos', '4cf1676', 'https://github.com/AlexandreRouma/libfobos', gh('AlexandreRouma/libfobos', '4cf1676c94860d24b231edb06743e3efbb26fd74', 'LICENSE')),
    ('libhackrf', '1cfe7df', 'https://github.com/greatscottgadgets/hackrf', gh('greatscottgadgets/hackrf', '1cfe7dfe98d333450217d50e3f3a1ad0702e000f', 'COPYING')),
    ('libhydrasdr', '6e4014f', 'https://github.com/hydrasdr/hydrasdr-host', gh('hydrasdr/hydrasdr-host', '6e4014fe8db88b31a7c0f22faf042b63b4295d89', 'libhydrasdr/LICENSE.md')),
    ('libiio', '0.24', 'https://github.com/analogdevicesinc/libiio', gh('analogdevicesinc/libiio', 'v0.24', 'COPYING.txt')),
    ('libmirisdr-5', 'b4e6ffa', 'https://github.com/ericek111/libmirisdr-5', gh('ericek111/libmirisdr-5', 'b4e6ffaf81fac1b74646537cdfee29580817ef74', 'COPYING')),
    ('libogg', '1.3.6', 'https://github.com/xiph/ogg', gh('xiph/ogg', 'v1.3.6', 'COPYING')),
    ('libperseus-sdr', 'ffb471c', 'https://github.com/bubnikv/libperseus-sdr', gh('bubnikv/libperseus-sdr', 'ffb471cea64d4f2b8403e67bbaecda3117c8a690', 'COPYING.LESSER')),
    ('librtlsdr', '797f814', 'https://github.com/osmocom/rtl-sdr', gh('osmocom/rtl-sdr', '797f8143266d983c56d8f35d2d442527529dd8a5', 'COPYING')),
    ('libusb', '', 'https://github.com/libusb/libusb', gh('libusb/libusb-cmake', 'c8477c10ac2ac6b1718d4d498e102b9f18b776f5', 'LICENSE', 'libusb/COPYING')),
    ('libxml2', '2.9.14', 'https://gitlab.gnome.org/GNOME/libxml2', gh('GNOME/libxml2', 'v2.9.14', 'Copyright')),
    ('LimeSuite', '23.11.0', 'https://github.com/myriadrf/LimeSuite', gh('myriadrf/LimeSuite', 'v23.11.0', 'COPYING')),
    ('Mbed TLS', '3.6.7', 'https://github.com/Mbed-TLS/mbedtls', gh('Mbed-TLS/mbedtls', 'v3.6.7', 'LICENSE')),
    ('Opus', '1.5.2', 'https://github.com/xiph/opus', gh('xiph/opus', 'v1.5.2', 'COPYING')),
    ('PortAudio', '19.7.0', 'https://github.com/PortAudio/portaudio', gh('PortAudio/portaudio', 'v19.7.0', 'LICENSE.txt')),
    ('pthreads4w', '3.0.0', 'https://sourceforge.net/projects/pthreads4w',
     archive('https://downloads.sourceforge.net/project/pthreads4w/pthreads4w-code-v3.0.0.zip', 'LICENSE', 'NOTICE')),
    ('RtAudio', '2f2fca4', 'https://github.com/thestk/rtaudio', gh('thestk/rtaudio', '2f2fca4502d506abc50f6d4473b2836d24cfb1e3', 'LICENSE')),
    ('sondedump', '5286555', 'https://github.com/dbdexter-dev/sondedump', gh('dbdexter-dev/sondedump', '528655564f34a42e464334fced4f7e89f9bfee46', 'LICENSE')),
    ('spdlog', '1.15.0', 'https://github.com/gabime/spdlog', gh('gabime/spdlog', 'v1.15.0', 'LICENSE')),
    ('VOLK', 'e97e1fb', 'https://github.com/gnuradio/volk', gh('gnuradio/volk', 'e97e1fb4dadd138a5842a8bcffb38e57b067334d', 'COPYING')),
    ('zlib', '1.3.1', 'https://github.com/madler/zlib', gh('madler/zlib', 'v1.3.1', 'LICENSE')),
    ('Zstandard', '1.5.7', 'https://github.com/facebook/zstd', gh('facebook/zstd', 'v1.5.7', 'LICENSE')),

    # Android runtime
    ('Oboe', '1.10.0', 'https://github.com/google/oboe', gh('google/oboe', '1.10.0', 'LICENSE')),
    ('AndroidX AppCompat', '1.7.0', 'https://developer.android.com/jetpack/androidx', [APACHE_2]),
    ('Kotlin standard library', '', 'https://github.com/JetBrains/kotlin', [APACHE_2]),
    ('LLVM libc++', '', 'https://github.com/llvm/llvm-project', gh('llvm/llvm-project', 'llvmorg-19.1.0', 'libcxx/LICENSE.TXT')),
]

# Components with no license text to reproduce here
NOTES = [
    ('SDRplay API', 'Proprietary binary distributed by SDRplay Limited under its own license, see https://www.sdrplay.com/api/.'),
    ('librfnm', 'https://github.com/AlexandreRouma/librfnm publishes no license file.'),
    ('Natural Earth', 'The country map data is in the public domain, see https://www.naturalearthdata.com/about/terms-of-use/.'),
]

HEADER = """SDRIAK third-party notices

SDRIAK itself is licensed under the GNU General Public License, version 3, see
GPL-3.0.txt in this directory. It includes the third-party software listed
below, each under its own license. Not every package contains every component.

This file is generated by scripts/generate_third_party_notices.py.
"""


def decode(data):
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return data.decode('latin-1')


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'sdriak-notices'})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def read_archive(url, member):
    with zipfile.ZipFile(io.BytesIO(download(url))) as archive_file:
        name = next(n for n in archive_file.namelist() if n.count('/') <= 1 and n.endswith('/' + member))
        return decode(archive_file.read(name))


def read_excerpt(path, start, end):
    lines = decode((ROOT / path).read_bytes()).splitlines()
    first = next(i for i, line in enumerate(lines) if re.search(start, line))
    last = next(i for i in range(first, len(lines)) if re.search(end, lines[i]))
    # Drop the comment decoration of the host file
    return '\n'.join(re.sub(r'^\s*(//|\*)\s?', '', line) for line in lines[first:last + 1])


def read_source(source):
    if isinstance(source, str):
        return decode(download(source))
    if source[0] == 'file':
        return decode((ROOT / source[1]).read_bytes())
    if source[0] == 'archive':
        return read_archive(*source[1:])
    return read_excerpt(*source[1:])


def main():
    seen = {}
    sections = []
    for name, version, homepage, sources in LIBRARIES:
        title = f'{name} {version}'.rstrip()
        print(title, file=sys.stderr)
        parts = []
        for source in sources:
            text = read_source(source).replace('\r\n', '\n').strip()
            key = ' '.join(text.split())
            if key in seen:
                parts.append(f'Same license text as {seen[key]}.')
            else:
                seen[key] = name
                parts.append(text)
        sections.append(f'{title}\n{homepage}\n\n' + '\n\n'.join(parts))

    notes = '\n'.join(f'{name}: {text}' for name, text in NOTES)
    rule = '\n\n' + '=' * 78 + '\n\n'
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER + rule + notes + rule + rule.join(sections) + '\n')


if __name__ == '__main__':
    main()
