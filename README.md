# LightGeo

[![CI/CD](https://github.com/s1lentq/LightGeo/actions/workflows/ci.yml/badge.svg)](https://github.com/s1lentq/LightGeo/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/s1lentq/LightGeo)](https://github.com/s1lentq/LightGeo/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Fast, zero-allocation IPv4 geolocation library and database compiler.

A lightweight C++ alternative to [libmaxminddb](https://github.com/maxmind/libmaxminddb) for autonomous client applications and edge computing. Powered by the GeoLite2-Country dataset.


## Scope

LightGeo is strictly minimal. It provides only the essential fields for IP-to-country resolution: IPv4 blocks, continent codes, country ISO codes, and localized country names.


## System Requirements

* C++ Header (lightgeo.hpp): C++14 minimum.
* Database Compiler: C++20 minimum.


## Performance Benchmark

Tested against the official MaxMind libmaxminddb engine using 1,000,000 random IPv4 addresses.

```text
> LightGeo Benchmark
  Total time: 0.0134 seconds
  Avg time per lookup: 13.41 ns
  Speed: 74,592,759 lookups/sec

> GeoLite2 Benchmark (libmaxminddb)
  Total time: 0.3229 seconds
  Avg time per lookup: 322.92 ns
  Speed: 3,096,731 lookups/sec

Result: LightGeo is ~24x faster.
```


## Core Architecture

* Header-only: Requires only `lightgeo.hpp`. No external library compilation or linking.

* Zero Heap Allocation: The lookup process operates entirely on the stack and memory-mapped files.

* Cache-Friendly Layout: Utilizes a /16 Look-Up Table (LUT) index and flat arrays to maximize L1/L2 cache locality.

* Built-in Localization: Contains native localized country names.

* Up-to-date Upstream: The database is compiled directly from the latest GeoLite2 data to ensure accuracy.


## Integration
Pre-compiled databases and headers are available on the [Releases page](../../releases).

```cpp
#include <iostream>
#include <lightgeo.hpp>

int main() {
    LightGeo::Db geoDb;
    
    if (!geoDb.Open("LightGeo.db")) {
        std::cerr << "Failed to load database\n";
        return 1;
    }

    // IP address lookup requires Host Byte Order
    // Example: 8.8.8.8 -> 134744072
    auto result = geoDb.Lookup(134744072); 
    
    if (result) {
        std::cout << "Country: " << result->country_iso_code << "\n";

        if (auto en = result["en"]) {
            std::cout << "Name (EN): " << en->country_name << "\n";
        }
        if (auto ru = result["ru"]) {
            std::cout << "Name (RU): " << ru->country_name << "\n";
        }
    }

    return 0;
}
```


## Advanced Usage
For hot-paths, pre-binding a specific language via LocaleView eliminates string lookups during IP resolution.
```cpp
LightGeo::LocaleView geoView;
geoView.Init(&geoDb, "en");

void OnClientConnect(uint32_t ip) {
    auto result = geoView.Lookup(ip); 
    if (result) {
        printf("Connection from: %s\n", result->country_name);
    }
}
```


## Compiler Build Instructions
For manual database generation from raw CSV data.

```bash
git clone https://github.com/s1lentq/LightGeo.git
cd LightGeo
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

./build/lightgeo -l locations-en.csv -l locations-ru.csv -b blocks-ipv4.csv -o CustomGeo.db
```
