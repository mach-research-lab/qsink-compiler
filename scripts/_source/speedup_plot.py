"""Draw the two Figure 6 panels from the values in the paper."""

from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


METHODS = {
    "qsink": ("qSink", "#0072B2"),
    "swiper": ("SWIPER", "#009E73"),
    "qsink-swiper": ("qSink-SWIPER", "#D55E00"),
}


def write_plot(rows: list[dict[str, str]], path: Path) -> None:
    series = defaultdict(dict)
    for row in rows:
        series[row["method"]][int(row["code_distance"])] = (
            float(row["mean_idle_reduction_percent"]),
            float(row["geomean_speedup"]),
        )

    distances = sorted(series["qsink"])
    if not distances or any(sorted(series[method]) != distances for method in METHODS):
        raise ValueError("Figure 6 requires values for all three methods at each distance")

    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["Times New Roman", "Times", "Nimbus Roman No9 L", "DejaVu Serif"],
            "font.size": 8,
            "axes.labelsize": 8,
            "axes.titlesize": 8,
            "xtick.labelsize": 7,
            "ytick.labelsize": 7,
            "legend.fontsize": 7,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "savefig.dpi": 300,
            "figure.dpi": 300,
            "axes.linewidth": 0.6,
            "grid.linewidth": 0.35,
            "patch.linewidth": 0.35,
        }
    )

    fig, axes = plt.subplots(2, 1, figsize=(3.50, 3.45), sharex=True, constrained_layout=True)
    panels = (
        ("Critical-path idle-time reduction", "Mean reduction (%)", 0.0, 1.10),
        ("Total execution time speedup", "Geomean speedup", 1.0, 1.0),
    )
    # The published chart uses different values for the bar width and offsets.
    bar_slot = 1.45 / len(METHODS)
    bar_width = 0.52 * 0.92
    for index, (ax, (title, ylabel, minimum, padding)) in enumerate(zip(axes, panels)):
        maximum = max(series[method][distance][index] for method in METHODS for distance in distances)
        for method_index, (method, (label, color)) in enumerate(METHODS.items()):
            offset = (method_index - 1) * bar_slot
            ax.bar(
                [distance + offset for distance in distances],
                [series[method][distance][index] for distance in distances],
                width=bar_width,
                label=label,
                color=color,
                edgecolor="0.25",
                linewidth=0.35,
            )
        ax.axhline(0.0, color="0.25", linewidth=0.45)
        ax.set_ylabel(ylabel)
        ax.set_title(title, pad=2)
        ax.set_ylim(minimum, maximum * padding)
        ax.set_xticks(distances)
        ax.set_xlim(min(distances) - 1.1, max(distances) + 1.1)
        ax.tick_params(axis="both", which="both", length=2.2, width=0.5, pad=1.5)
        ax.grid(True, axis="y", linestyle="--", linewidth=0.35, alpha=0.35)
        ax.set_axisbelow(True)

    axes[1].legend(frameon=False, loc="upper left", ncol=1, handlelength=1.0)
    axes[1].set_xlabel("Code distance")
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
