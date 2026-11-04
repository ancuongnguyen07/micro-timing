import csv
import os
from os import path

CSV_DIR = path.join(path.dirname(__file__), "../odata")

EXPECTED_HEADER = (
    "timestamp,cpu_id,sibling_cpu,sibling_state,sibling_load,sibling_cmd,"
    "fill_entry,"
    "sqrt_delay,unroll,warmup,iterations,rdpru_sel,"
    "empty_min,empty_median,empty_mean,empty_stddev,empty_p99,empty_max,"
    "body_min,body_median,body_mean,body_stddev,body_p99,body_max,"
    "net_median_ticks,net_ticks_per_unroll,"
    "pmc_event,pmc_regions,pmc_empty_total,pmc_body_total,pmc_net_total,"
    "pmc_net_per_region,pmc_net_per_unroll"
)
EXPECTED_NUM_COLUMNS = len(EXPECTED_HEADER.split(","))

# Files written before the `sibling_cmd` column existed are still valid.
LEGACY_HEADER = EXPECTED_HEADER.replace("sibling_load,sibling_cmd,", "sibling_load,")
LEGACY_NUM_COLUMNS = len(LEGACY_HEADER.split(","))

if __name__ == "__main__":
    csv_file_ls = [f for f in os.listdir(CSV_DIR) if f.endswith(".csv")]
    if not csv_file_ls:
        raise FileNotFoundError(f"No CSV files found in {CSV_DIR}")

    for csv_file in csv_file_ls:
        csv_path = path.join(CSV_DIR, csv_file)
        print(f"Checking CSV file: {csv_path}")
        with open(csv_path, "r") as f:
            header = f.readline().strip()

            assert header in (EXPECTED_HEADER, LEGACY_HEADER), (
                f"CSV file {csv_file} has an unexpected header:\n{header}\nExpected:\n{EXPECTED_HEADER}"
            )
            num_expected = (
                EXPECTED_NUM_COLUMNS
                if header == EXPECTED_HEADER
                else LEGACY_NUM_COLUMNS
            )

            # Check consistency of the number of columns in each row.
            # csv.reader, not str.split: `sibling_cmd` is a quoted field that
            # may itself contain commas.
            for line_num, row in enumerate(csv.reader(f), start=2):
                assert len(row) == num_expected, (
                    f"CSV file {csv_file} has an inconsistent number of columns at line {line_num}: "
                    f"expected {num_expected}, got {len(row)}"
                )
        print(f"CSV file {csv_file} PASSED the ALL tests.")

    print(f"All CSV files in {CSV_DIR} PASSED the ALL tests.")
