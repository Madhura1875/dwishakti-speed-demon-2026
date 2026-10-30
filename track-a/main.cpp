#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

struct Group {
    vector<uint32_t> latencies;
    uint64_t errors = 0;
};

array<uint32_t, 3> getPercentiles(vector<uint32_t>& values) {
    size_t n = values.size();

    size_t i50 = (50 * n + 99) / 100 - 1;
    size_t i95 = (95 * n + 99) / 100 - 1;
    size_t i99 = (99 * n + 99) / 100 - 1;

    auto first = values.begin();
    auto last = values.end();

    nth_element(first, first + i50, last);

    if (i95 > i50) {
        nth_element(first + i50 + 1, first + i95, last);
    }

    if (i99 > i95) {
        nth_element(first + i95 + 1, first + i99, last);
    }

    return {values[i50], values[i95], values[i99]};
}

int main(int argc, char* argv[]) {
    string dataDir = argc > 1 ? argv[1] : "/data";
    string outDir  = argc > 2 ? argv[2] : "/out";

    ifstream input(dataDir + "/requests.log");

    if (!input) {
        cerr << "Cannot open requests.log\n";
        return 1;
    }

    unordered_map<string, Group> groups;
    groups.reserve(200000);

    string line;

    while (getline(input, line)) {
        size_t s1 = line.find(' ');
        size_t s2 = line.find(' ', s1 + 1);
        size_t s3 = line.find(' ', s2 + 1);
        size_t s4 = line.find(' ', s3 + 1);

        string hour = line.substr(0, 13);
        string endpoint = line.substr(s2 + 1, s3 - s2 - 1);

        string key = hour;
        key.push_back('\t');
        key += endpoint;

        auto it = groups.find(key);

        if (it == groups.end()) {
            it = groups.emplace(move(key), Group{}).first;
        }

        Group& group = it->second;

        int status =
            (line[s3 + 1] - '0') * 100 +
            (line[s3 + 2] - '0') * 10 +
            (line[s3 + 3] - '0');

        uint32_t latency = 0;

        const char* first = line.data() + s4 + 1;
        const char* last = line.data() + line.size();

        from_chars(first, last, latency);

        group.latencies.push_back(latency);

        if (status >= 500 && status <= 599) {
            group.errors++;
        }
    }

    input.close();

    using Entry = unordered_map<string, Group>::value_type;

    vector<const Entry*> ordered;
    ordered.reserve(groups.size());

    for (const auto& entry : groups) {
        ordered.push_back(&entry);
    }

    sort(ordered.begin(), ordered.end(),
         [](const Entry* a, const Entry* b) {
             return a->first < b->first;
         });

    filesystem::create_directories(outDir);

    ofstream output(outDir + "/summary.txt");

    if (!output) {
        cerr << "Cannot create summary.txt\n";
        return 1;
    }

    for (const Entry* entry : ordered) {
        const string& key = entry->first;
        Group& group = const_cast<Group&>(entry->second);

        auto& values = group.latencies;

        auto percentiles = getPercentiles(values);

        uint64_t count = values.size();
        uint64_t errors = group.errors;

        uint64_t ppm = errors * 1000000ULL / count;

        output << key.substr(0, 13) << ":00:00Z "
               << key.substr(14) << " "
               << count << " "
               << percentiles[0] << " "
               << percentiles[1] << " "
               << percentiles[2] << " "
               << errors << " "
               << ppm << "\n";
    }

    output.close();

    return 0;
}