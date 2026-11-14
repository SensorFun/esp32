"""Keep safe Janus IDs as decimal JSON integers, even at the 53-bit boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class JanusIdTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Native C++ compiler required")
    def test_integer_formatting_and_invalid_ids(self):
        source = r'''
#include "janus_id.h"
#include <cassert>
#include <cstdlib>
#include <limits>
#include <string>
int main() {
    char out[24]{};
    for (double value : {1.0, 2147483648.0, 1000000000000000.0, 8000000000000010.0, 9007199254740991.0}) {
        assert(format_janus_id(value, out, sizeof(out)));
        std::string text(out);
        assert(text.find_first_not_of("0123456789") == text.npos);
        assert(static_cast<double>(std::strtoull(out, nullptr, 10)) == value);
    }
    assert(std::string(out) == "9007199254740991");
    for (double value : {0.0, -1.0, 1.5, 9007199254740992.0,
                        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        assert(!valid_janus_id(value));
        assert(!format_janus_id(value, out, sizeof(out)));
    }
    assert(!format_janus_id(1000, out, 3));
    assert(!format_janus_id(1, nullptr, 0));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "check.cpp").write_text(source)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "projects/webrtc/main"), str(path / "check.cpp"),
                            "-o", str(path / "check")], check=True)
            subprocess.run([str(path / "check")], check=True)
