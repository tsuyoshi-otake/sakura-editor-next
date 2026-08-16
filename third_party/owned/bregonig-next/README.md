[![Build status](https://ci.appveyor.com/api/projects/status/plm9o9tkcf27g7hc/branch/master?svg=true)](https://ci.appveyor.com/project/k-takata/bregonig/branch/master)

# bregonig.dll

This is a source code package of bregonig.dll regular expression library.

Binary packages and documents are available at the following site:  
http://k-takata.o.oo7.jp/mysoft/bregonig.html (Japanese)

Bregonig.dll is a regular expression library compatible with bregexp.dll.
Bregexp.dll was widely used in Japanese Win32 applications, but the regexp
engine was very old. (It seems to be a modified version of Perl 5.00x.)
On the other hand, bregonig.dll uses Oniguruma (or Onigmo) to support
more powerful regexp patterns.

## CMake (canonical build)

```text
cmake -S . -B build -A x64 -DONIGMO_SOURCE_DIR=<path-to-onigmo-next>
cmake --build build --config Release
ctest --test-dir build -C Release
```

`find_package(Onigmo)` is used when `ONIGMO_SOURCE_DIR` is empty. The installed
package exports `bregonig::bregonig` with the DLL, import library, and
`bregexp.h`. GitHub Actions builds Win32 and x64, runs AddressSanitizer, and
smokes the libFuzzer harness.

The nmake `src/Makefile` remains as historical documentation. Product builds
must not reconstruct the DLL through a consumer-local nmake portfile.


You may distribute under the terms of either the GNU General Public
License or the Artistic License.

## References

* bregexp.dll:  
  http://www.hi-ho.ne.jp/babaq/bregexp.html (Japanese)

* Oniguruma:  
  http://github.com/kkos/oniguruma  
  Compatible with Perl 5.8's regexp patterns.

* Onigmo (Oniguruma-mod):  
  https://github.com/k-takata/Onigmo  
  Compatible with Perl 5.14's regexp patterns.
