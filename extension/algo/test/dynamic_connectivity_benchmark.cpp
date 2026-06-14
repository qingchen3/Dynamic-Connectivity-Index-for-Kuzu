#include "common/dynamic_connectivity_index_factory.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace kuzu::algo_extension;

namespace {

enum class ValidateMode {
    NONE,
    EXPECTED,
};

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isCommentOrEmpty(const std::string& line) {
    auto t = trim(line);
    return t.empty() || t[0] == '#';
}

bool isInsertOp(const std::string& op) {
    return op == "INS" || op == "ins" || op == "insert" || op == "i" || op == "+";
}

bool isDeleteOp(const std::string& op) {
    return op == "DEL" || op == "del" || op == "delete" || op == "d" || op == "-";
}

struct TraceOperation {
    std::string op;
    int64_t u = 0;
    int64_t v = 0;
    bool hasExpectedConnected = false;
    bool expectedConnected = false;
};

bool parseLine(const std::string& line, TraceOperation& operation) {
    std::stringstream ss(line);

    std::string op;
    int64_t u = 0;
    int64_t v = 0;

    if (!(ss >> op >> u >> v)) {
        return false;
    }

    operation.op = lower(op);
    operation.u = u;
    operation.v = v;

    int expected = 0;
    if (ss >> expected) {
        operation.hasExpectedConnected = true;
        operation.expectedConnected = expected != 0;
    } else {
        operation.hasExpectedConnected = false;
        operation.expectedConnected = false;
    }

    return true;
}

ValidateMode parseValidateMode(const std::string& arg) {
    if (arg == "--validate=none") {
        return ValidateMode::NONE;
    }
    if (arg == "--validate=expected") {
        return ValidateMode::EXPECTED;
    }
    throw std::runtime_error("Unknown validation option: " + arg);
}

void printUsage(const char* programName) {
    std::cerr << "Usage:\n"
              << "  " << programName << " <stree|dtree> <trace_file> [--validate=expected|--validate=none]\n\n"
              << "Trace format:\n"
              << "  OP u v [expected_connected]\n\n"
              << "Examples:\n"
              << "  INS 1 2\n"
              << "  INS 1 2 1\n"
              << "  DEL 1 2\n"
              << "  DEL 1 2 0\n\n"
              << "Validation modes:\n"
              << "  --validate=expected   validate only lines with the optional 4th field, default\n"
              << "  --validate=none       do not run validation checks\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string method = argv[1];
    const std::string traceFile = argv[2];

    ValidateMode validateMode = ValidateMode::EXPECTED;
    if (argc == 4) {
        try {
            validateMode = parseValidateMode(argv[3]);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    auto index = createDynamicConnectivityIndex(method);

    std::ifstream input(traceFile);
    if (!input.is_open()) {
        std::cerr << "Failed to open trace file: " << traceFile << "\n";
        return 1;
    }

    uint64_t totalLines = 0;
    uint64_t processedOps = 0;
    uint64_t skippedLines = 0;

    uint64_t insertCount = 0;
    uint64_t deleteCount = 0;

    uint64_t validationChecks = 0;
    uint64_t validationErrors = 0;

    double insertMicros = 0.0;
    double deleteMicros = 0.0;
    double validationMicros = 0.0;

    std::string line;
    while (std::getline(input, line)) {
        totalLines++;

        if (isCommentOrEmpty(line)) {
            continue;
        }

        TraceOperation op;
        try {
            if (!parseLine(line, op)) {
                skippedLines++;
                continue;
            }
        } catch (const std::exception&) {
            skippedLines++;
            continue;
        }

        if (isInsertOp(op.op)) {
            auto start = std::chrono::steady_clock::now();
            index->insertEdge(op.u, op.v);
            auto end = std::chrono::steady_clock::now();

            insertMicros += std::chrono::duration<double, std::micro>(end - start).count();
            insertCount++;
            processedOps++;
        } else if (isDeleteOp(op.op)) {
            auto start = std::chrono::steady_clock::now();
            index->deleteEdge(op.u, op.v);
            auto end = std::chrono::steady_clock::now();

            deleteMicros += std::chrono::duration<double, std::micro>(end - start).count();
            deleteCount++;
            processedOps++;
        } else {
            skippedLines++;
            continue;
        }

        if (validateMode == ValidateMode::EXPECTED && op.hasExpectedConnected) {
            auto start = std::chrono::steady_clock::now();
            bool actualConnected = index->connected(op.u, op.v);
            auto end = std::chrono::steady_clock::now();

            validationMicros += std::chrono::duration<double, std::micro>(end - start).count();
            validationChecks++;

            if (actualConnected != op.expectedConnected) {
                validationErrors++;

                if (validationErrors <= 10) {
                    std::cerr << "Validation error at line " << totalLines
                              << ": op=" << op.op
                              << " u=" << op.u
                              << " v=" << op.v
                              << " expected=" << op.expectedConnected
                              << " actual=" << actualConnected << "\n";
                }
            }
        }
    }

    std::cout << "========== Dynamic Connectivity Benchmark ==========\n";
    std::cout << "Method: " << index->getName() << "\n";
    std::cout << "Trace file: " << traceFile << "\n";
    std::cout << "Validation mode: "
              << (validateMode == ValidateMode::EXPECTED ? "expected" : "none") << "\n\n";

    std::cout << "Total lines: " << totalLines << "\n";
    std::cout << "Processed operations: " << processedOps << "\n";
    std::cout << "Skipped lines: " << skippedLines << "\n";
    std::cout << "Number of nodes created: " << index->getNumNodes() << "\n\n";

    std::cout << "Insertions:\n";
    std::cout << "  Count: " << insertCount << "\n";
    std::cout << "  Total time: " << insertMicros / 1000000.0 << " seconds\n";
    if (insertCount > 0) {
        std::cout << "  Avg time/op: " << insertMicros / insertCount << " us\n";
    }

    std::cout << "\nDeletions:\n";
    std::cout << "  Count: " << deleteCount << "\n";
    std::cout << "  Total time: " << deleteMicros / 1000000.0 << " seconds\n";
    if (deleteCount > 0) {
        std::cout << "  Avg time/op: " << deleteMicros / deleteCount << " us\n";
    }

    std::cout << "\nValidation:\n";
    std::cout << "  Checks: " << validationChecks << "\n";
    std::cout << "  Errors: " << validationErrors << "\n";
    std::cout << "  Total time: " << validationMicros / 1000000.0 << " seconds\n";
    if (validationChecks > 0) {
        std::cout << "  Avg time/check: " << validationMicros / validationChecks << " us\n";
    }

    if (validationErrors > 0) {
        return 2;
    }

    return 0;
}
