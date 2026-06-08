if(NOT DEFINED ASTRALOG_EXE)
    message(FATAL_ERROR "ASTRALOG_EXE is required")
endif()

if(NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "FIXTURE_DIR is required")
endif()

if(NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "OUTPUT_ROOT is required")
endif()

set(SENSORS_FILE "${FIXTURE_DIR}/sensors.yaml")
set(RULES_FILE "${FIXTURE_DIR}/rules.json")
set(CSV_FILE "${FIXTURE_DIR}/telemetry.csv")
set(EXPECTED_VALID_FILE "${FIXTURE_DIR}/expected_valid_data.csv")
set(EXPECTED_ALARMS_FILE "${FIXTURE_DIR}/expected_alarms.log")

file(READ "${EXPECTED_VALID_FILE}" EXPECTED_VALID)
file(READ "${EXPECTED_ALARMS_FILE}" EXPECTED_ALARMS)

function(assert_outputs_match output_dir context)
    set(valid_file "${output_dir}/valid_data.csv")
    set(alarms_file "${output_dir}/alarms.log")

    if(NOT EXISTS "${valid_file}")
        message(FATAL_ERROR "${context}: missing valid_data.csv")
    endif()

    if(NOT EXISTS "${alarms_file}")
        message(FATAL_ERROR "${context}: missing alarms.log")
    endif()

    file(READ "${valid_file}" actual_valid)
    file(READ "${alarms_file}" actual_alarms)

    if(NOT actual_valid STREQUAL EXPECTED_VALID)
        message(FATAL_ERROR "${context}: valid_data.csv differs from golden output")
    endif()

    if(NOT actual_alarms STREQUAL EXPECTED_ALARMS)
        message(FATAL_ERROR "${context}: alarms.log differs from golden output")
    endif()
endfunction()

function(run_case threads batch_size benchmark_suffix)
    set(case_name "threads_${threads}_batch_${batch_size}${benchmark_suffix}")
    set(output_dir "${OUTPUT_ROOT}/${case_name}")
    file(REMOVE_RECURSE "${output_dir}")
    file(MAKE_DIRECTORY "${output_dir}")

    set(args
        "--csv" "${CSV_FILE}"
        "--rules" "${RULES_FILE}"
        "--sensors" "${SENSORS_FILE}"
        "--output-dir" "${output_dir}"
        "--threads" "${threads}"
        "--batch-strategy" "count"
        "--batch-size" "${batch_size}"
    )

    if(benchmark_suffix STREQUAL "_benchmark")
        list(APPEND args "--benchmark")
    endif()

    execute_process(
        COMMAND "${ASTRALOG_EXE}" ${args}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${case_name}: executable failed with code ${result}\n"
            "stdout:\n${stdout}\n"
            "stderr:\n${stderr}")
    endif()

    assert_outputs_match("${output_dir}" "${case_name}")
endfunction()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")

# Golden-file and metamorphic cases. The same fixture must produce byte-identical
# outputs when OpenMP thread count and count-based batch size change.
set(CASES
    "1,1000"
    "2,1"
    "3,2"
    "4,3"
    "8,4"
    "12,5"
)

foreach(case_spec IN LISTS CASES)
    string(REPLACE "," ";" case_parts "${case_spec}")
    list(GET case_parts 0 threads)
    list(GET case_parts 1 batch_size)
    run_case("${threads}" "${batch_size}" "_benchmark")
endforeach()

# One non-benchmark case verifies the executable integration with audit-file
# generation. The timestamped file names are variable, so only count/presence is
# asserted here.
run_case("4" "3" "")

file(GLOB audit_files "${OUTPUT_ROOT}/threads_4_batch_3/batches/batch_*.txt")
list(LENGTH audit_files audit_count)
if(NOT audit_count EQUAL 5)
    message(FATAL_ERROR
        "audit-file integration: expected 5 batch audit files, found ${audit_count}")
endif()
