//
// LightGeo Benchmark & Unit Tests
// Usage: lightgeo_bench <LightGeo.db> [GeoLite2-Country.mmdb]
//

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <string>

#include <lightgeo.hpp>

#ifdef USE_MAXMINDDB
#include <maxminddb.h>
#endif

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <LightGeo.db> [GeoLite2-Country.mmdb]" << std::endl;
		return 1;
	}

	std::string db_path = argv[1];
	std::string mmdb_path = (argc > 2) ? argv[2] : "GeoLite2-Country.mmdb";

	std::cout << "--- LightGeo Unit Tests & Benchmarks ---" << std::endl << std::endl;

	LightGeo::Db lgeo;
	if (!lgeo.Open(db_path.c_str())) {
		std::cerr << "CRITICAL ERROR: Could not load LightGeo database '" << db_path << "'" << std::endl;
		std::cerr << "Did you compile it first using LightGeoCompiler?" << std::endl;
		return 1;
	}

	std::cout << "LightGeo Database loaded successfully." << std::endl;
	std::cout << "  - Entries: " << lgeo.GetEntryCount() << ", Locations: " << lgeo.GetLocationCount() << std::endl;

	std::cout << "[1/3] Running Correctness Sanity Checks..." << std::endl;
	int passedTests = 0;

	const LightGeo::LookupResult dns_loc = lgeo.Lookup("8.8.8.8");
	if (dns_loc && dns_loc->country_iso_code[0] == 'U' && dns_loc->country_iso_code[1] == 'S') {
		passedTests++;
		std::cout << "  - DNS Test (8.8.8.8) passed (Resolved to US)." << std::endl;

		if (const LightGeo::LocaleData* en = dns_loc["en"]) {
			std::cout << "    > Text [en]: " << en->country_name << std::endl;
		}

		if (const LightGeo::LocaleData* ru = dns_loc["ru"]) {
			std::cout << "    > Text [ru]: " << ru->country_name << std::endl;
		}

	} else {
		std::cerr << "  - DNS Test failed! Expected: US" << std::endl;
	}

	if (passedTests == 0) {
		std::cerr << "Sanity checks failed! Aborting benchmark." << std::endl;
		return 1;
	}

	// test benchmark
	std::cout << "[2/3] Preparing 1,000,000 random IPs for benchmark..." << std::endl;
	const int NUM_LOOKUPS = 1000000;
	std::vector<uint32_t> randomIPs(NUM_LOOKUPS);

	std::mt19937 rng(1337);
	std::uniform_int_distribution<uint32_t> dist;
	for (int i = 0; i < NUM_LOOKUPS; i++) {
		randomIPs[i] = dist(rng);
	}

	std::cout << "[3/3] Running Performance Benchmarks..." << std::endl;
	std::cout << "--------------------------------------------------------" << std::endl;

	std::cout << std::endl << ">> LightGeo Benchmark" << std::endl;

	int lgeoFoundCount = 0;
	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < NUM_LOOKUPS; i++) {
		const LightGeo::LookupResult loc = lgeo.Lookup(randomIPs[i]);
		if (loc) {
			lgeoFoundCount++;
		}
	}
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> lgeoTimeElapsed = end - start;

	double lgeoTimePerLookupNS = (lgeoTimeElapsed.count() / NUM_LOOKUPS) * 1000000000.0;
	double lgeoLookupsPerSec = NUM_LOOKUPS / lgeoTimeElapsed.count();

	std::cout << "  - Total time: " << std::fixed << std::setprecision(4) << lgeoTimeElapsed.count() << " seconds" << std::endl;
	std::cout << "  - Avg time per lookup: " << std::setprecision(2) << lgeoTimePerLookupNS << " ns" << std::endl;
	std::cout << "  - Speed: " << std::setprecision(0) << lgeoLookupsPerSec << " lookups/sec" << std::endl;
	std::cout << "  - IPs resolved: " << lgeoFoundCount << " / " << NUM_LOOKUPS << std::endl;

#ifdef USE_MAXMINDDB
	std::cout <<  std::endl << ">> GeoLite2 Benchmark (libmaxminddb)" << std::endl;

	std::string mmdbPathStr = mmdb_path.empty() ? "GeoLite2-Country.mmdb" : mmdb_path;

	MMDB_s mmdb;
	int status = MMDB_open(mmdbPathStr.c_str(), MMDB_MODE_MMAP, &mmdb);

	if (status != MMDB_SUCCESS) {
		std::cerr << "  - WARNING: Failed to open " << mmdb_path << ": " << MMDB_strerror(status) << std::endl;
		std::cerr << "  - GeoLite2 benchmark skipped." << std::endl;
	} else {
		std::cout << "GeoLite2 Database loaded successfully." << std::endl;
		int mmdbFoundCount = 0;
		start = std::chrono::high_resolution_clock::now();

		for (int i = 0; i < NUM_LOOKUPS; i++) {
			struct sockaddr_in sa{};
			sa.sin_family = AF_INET;
			sa.sin_addr.s_addr = htonl(randomIPs[i]);

			int mmdb_error;
			MMDB_lookup_result_s result = MMDB_lookup_sockaddr(&mmdb, (struct sockaddr *)&sa, &mmdb_error);

			if (mmdb_error == MMDB_SUCCESS && result.found_entry) {
				MMDB_entry_data_s entry_data;
				int lookup_status = MMDB_get_value(&result.entry, &entry_data, "country", "iso_code", NULL);
				if (lookup_status == MMDB_SUCCESS && entry_data.has_data) {
					mmdbFoundCount++;
				}
			}
		}

		end = std::chrono::high_resolution_clock::now();
		MMDB_close(&mmdb);

		std::chrono::duration<double> mmdbTimeElapsed = end - start;
		double mmdbTimePerLookupNS = (mmdbTimeElapsed.count() / NUM_LOOKUPS) * 1000000000.0;
		double mmdbLookupsPerSec = NUM_LOOKUPS / mmdbTimeElapsed.count();

		std::cout << "  - Total time: " << std::fixed << std::setprecision(4) << mmdbTimeElapsed.count() << " seconds" << std::endl;
		std::cout << "  - Avg time per lookup: " << std::setprecision(2) << mmdbTimePerLookupNS << " ns" << std::endl;
		std::cout << "  - Speed: " << std::setprecision(0) << mmdbLookupsPerSec << " lookups/sec" << std::endl;
		std::cout << "  - IPs resolved: " << mmdbFoundCount << " / " << NUM_LOOKUPS << std::endl;
		std::cout << "========================================================" << std::endl;

		double t_lgeo = lgeoTimeElapsed.count();
		double t_mmdb = mmdbTimeElapsed.count();
		if (t_lgeo < t_mmdb) {
			double ratio = t_mmdb / t_lgeo;
			std::cout << "RESULT: LightGeo is " << std::fixed << std::setprecision(2)
						<< ratio << "x FASTER than GeoLite2!" << std::endl;
		} else if (t_mmdb < t_lgeo) {
			double ratio = t_lgeo / t_mmdb;
			std::cout << "RESULT: GeoLite2 is " << std::fixed << std::setprecision(2)
						<< ratio << "x FASTER than LightGeo!" << std::endl;
		} else {
			std::cout << "RESULT: Tie! Both engines performed at the exact same speed." << std::endl;
		}
		std::cout << "========================================================" << std::endl;
	}
#endif

	std::cout << "---------------------------------------" << std::endl;

	return 0;
}
