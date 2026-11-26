#!/usr/bin/env python3
"""Regenerate the paper's four data figures from the bundled CSV files."""

import argparse
import csv
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "artifact-files" / "figure-data"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"Plot data is empty in {path}")
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / "output" / "figures",
        help="Directory for generated PDFs (default: output/figures in the checkout).",
    )
    output_dir = parser.parse_args().output_dir.resolve()

    import matplotlib

    matplotlib.use("Agg")

    from _source import plot_fpga_resources as fpga
    from _source import run_statistics as statistics
    from _source import speedup_plot

    output_dir.mkdir(parents=True, exist_ok=True)

    idle_rows = read_csv(DATA / "f2_f3" / "figure2_idle_ratio.csv")
    statistics.write_code_volume_idle_ratio_by_distance_plot(
        idle_rows, output_dir / "figure2.pdf", decoder_bucket=2
    )

    gate_rows = read_csv(DATA / "f2_f3" / "figure3_gate_pairs_no_pauli.csv")
    gate_counts = Counter({row["gate_pair"]: int(row["count"]) for row in gate_rows})
    distribution = "first_two_gates_after_t_pair"
    statistics.write_critical_path_t_distribution_plot(
        gate_counts,
        output_dir / "figure3.pdf",
        distribution,
        title=statistics.no_pauli_plot_title(distribution),
    )

    speedup_rows = read_csv(DATA / "f6" / "figure6_summary.csv")
    speedup_plot.write_plot(speedup_rows, output_dir / "figure6.pdf")

    lut_rows = read_csv(DATA / "f7" / "fpga_lut_resources.csv")
    fetcher_values = {int(row["qsink_fetcher_luts"]) for row in lut_rows}
    if len(fetcher_values) != 1:
        raise ValueError("Figure 7 CSV has inconsistent qSink-fetcher LUT counts")
    fetcher_luts = fetcher_values.pop()
    distances = [int(row["code_distance"]) for row in lut_rows]
    blossom_luts = [int(row["micro_blossom_luts"]) for row in lut_rows]
    blossom_sources = [row["micro_blossom_source"] for row in lut_rows]
    fpga.validate_data(distances, blossom_luts, fetcher_luts)
    fpga.write_plot(
        output_dir / "figure7.pdf", distances, blossom_luts, blossom_sources, fetcher_luts
    )

    for figure in (2, 3, 6, 7):
        print(output_dir / f"figure{figure}.pdf")


if __name__ == "__main__":
    main()
