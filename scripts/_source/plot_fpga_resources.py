"""Paper figure support extracted from qSink's plot_fpga_resources module."""

from __future__ import annotations

import math
from pathlib import Path
from typing import Sequence
import matplotlib.pyplot as plt

IEEE_HPCA_COLUMN_WIDTH_IN = 3.50


IEEE_HPCA_STANDARD_FIGURE_HEIGHT_IN = 1.325


IEEE_HPCA_DPI = 300


IEEE_HPCA_FONT_SIZE = 8


IEEE_HPCA_TICK_FONT_SIZE = 7


PLOT_COLORS = {
    "micro_blossom": "#2ca02c",
    "qsink_fetcher": "#ff7f0e",
}


def validate_data(
    distances: Sequence[int],
    micro_blossom_luts: Sequence[int],
    qsink_fetcher_luts: int,
) -> None:
    if not distances:
        raise ValueError("At least one code distance is required.")
    if len(distances) != len(micro_blossom_luts):
        raise ValueError("Code distances and Micro Blossom LUT values must have equal lengths.")
    if any(right <= left for left, right in zip(distances, distances[1:])):
        raise ValueError("Code distances must be strictly increasing.")
    if any(value <= 0 for value in micro_blossom_luts):
        raise ValueError("Micro Blossom LUT values must be positive.")
    if any(
        right <= left
        for left, right in zip(micro_blossom_luts, micro_blossom_luts[1:])
    ):
        raise ValueError("Micro Blossom LUT values must be strictly increasing.")
    if qsink_fetcher_luts <= 0:
        raise ValueError("qSink-fetcher LUT usage must be positive.")



def configure_matplotlib() -> None:
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": [
                "Times New Roman",
                "Times",
                "Nimbus Roman No9 L",
                "DejaVu Serif",
            ],
            "font.size": IEEE_HPCA_FONT_SIZE,
            "axes.labelsize": IEEE_HPCA_FONT_SIZE,
            "axes.titlesize": IEEE_HPCA_FONT_SIZE,
            "xtick.labelsize": IEEE_HPCA_TICK_FONT_SIZE,
            "ytick.labelsize": IEEE_HPCA_TICK_FONT_SIZE,
            "legend.fontsize": IEEE_HPCA_TICK_FONT_SIZE,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "savefig.dpi": IEEE_HPCA_DPI,
            "figure.dpi": IEEE_HPCA_DPI,
            "axes.linewidth": 0.6,
            "grid.linewidth": 0.35,
            "lines.linewidth": 1.0,
            "patch.linewidth": 0.35,
            "mathtext.fontset": "dejavuserif",
        }
    )



def style_axis(ax) -> None:
    ax.tick_params(axis="both", which="both", length=2.2, width=0.5, pad=1.5)
    for spine in ax.spines.values():
        spine.set_linewidth(0.6)
    ax.grid(True, which="major", axis="both", linestyle="--", linewidth=0.35, alpha=0.35)
    ax.grid(True, which="minor", axis="y", linestyle="--", linewidth=0.35, alpha=0.18)
    ax.set_axisbelow(True)



def format_lut_tick(value: float, _position: int) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:g}M"
    if value >= 1_000:
        return f"{value / 1_000:g}k"
    return f"{value:g}"



def write_plot(
    path: Path,
    distances: Sequence[int],
    micro_blossom_luts: Sequence[int],
    micro_blossom_sources: Sequence[str],
    qsink_fetcher_luts: int,
) -> None:
    configure_matplotlib()
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

    path.parent.mkdir(parents=True, exist_ok=True)
    qsink_values = [qsink_fetcher_luts] * len(distances)
    last_measured_index = max(
        index
        for index, source in enumerate(micro_blossom_sources)
        if source == "measured"
    )
    measured_distances = distances[: last_measured_index + 1]
    measured_luts = micro_blossom_luts[: last_measured_index + 1]
    extrapolated_distances = distances[last_measured_index:]
    extrapolated_luts = micro_blossom_luts[last_measured_index:]

    fig, ax = plt.subplots(
        figsize=(IEEE_HPCA_COLUMN_WIDTH_IN, IEEE_HPCA_STANDARD_FIGURE_HEIGHT_IN),
        dpi=IEEE_HPCA_DPI,
        constrained_layout=True,
    )
    ax.plot(
        measured_distances,
        measured_luts,
        color=PLOT_COLORS["micro_blossom"],
        marker="s",
        markersize=4.0,
        markeredgecolor="0.25",
        markeredgewidth=0.35,
        linestyle="-",
        label="Micro Blossom",
    )
    ax.plot(
        extrapolated_distances,
        extrapolated_luts,
        color=PLOT_COLORS["micro_blossom"],
        marker="s",
        markersize=4.0,
        markeredgecolor="0.25",
        markeredgewidth=0.35,
        linestyle="--",
        label="_nolegend_",
    )
    ax.plot(
        distances,
        qsink_values,
        color=PLOT_COLORS["qsink_fetcher"],
        marker="o",
        markersize=3.8,
        markeredgecolor="0.25",
        markeredgewidth=0.35,
        linestyle="-",
        label="qSink-fetcher",
    )

    all_lut_values = [*micro_blossom_luts, *qsink_values]
    lower_decade = 10 ** math.floor(math.log10(min(all_lut_values)))
    upper_decade = 10 ** math.ceil(math.log10(max(all_lut_values)))
    ax.set_yscale("log")
    ax.set_ylim(0.8 * lower_decade, 1.2 * upper_decade)
    ax.yaxis.set_major_locator(LogLocator(base=10))
    ax.yaxis.set_major_formatter(FuncFormatter(format_lut_tick))
    ax.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5)))
    ax.yaxis.set_minor_formatter(NullFormatter())

    ax.set_title("FPGA LUT Usage by Code Distance", pad=2)
    ax.set_xlabel("Code distance $d$")
    ax.set_ylabel("LUT usage")
    ax.set_xticks(distances)
    ax.set_xlim(min(distances) - 0.6, max(distances) + 0.6)
    ax.legend(
        frameon=False,
        loc="center right",
        handlelength=1.8,
    )
    style_axis(ax)

    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
