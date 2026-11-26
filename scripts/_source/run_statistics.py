"""Paper figure support extracted from qSink's run_statistics module."""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

DEFAULT_DISTANCE_TICKS = tuple(range(11, 32, 2))


PLOT_COLORS = (
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
    "#bcbd22",
    "#17becf",
)


CODE_VOLUME_RATIO_COLOR = "#009E73"


CRITICAL_PATH_DISTRIBUTION_COLOR = "#0072B2"


IEEE_HPCA_TEXT_WIDTH_IN = 7.10


IEEE_HPCA_DPI = 300


IEEE_HPCA_FONT_SIZE = 8


IEEE_HPCA_TICK_FONT_SIZE = 7


IEEE_HPCA_MIN_TICK_FONT_SIZE = 5


IEEE_HPCA_STANDARD_FIGURE_HEIGHT_IN = 2.65


IEEE_HPCA_COMPACT_FIGURE_HEIGHT_IN = 2.35


CRITICAL_PATH_T_DISTRIBUTION_TITLES = {
    "gates_between_successive_t": "Critical-Path Gates Between Successive T Gates",
    "first_gate_after_t": "First Critical-Path Gate After T",
    "second_gate_after_t": "Second Critical-Path Gate After T",
    "first_two_gates_after_t_pair": "First Two Critical-Path Gates After T",
}


CRITICAL_PATH_T_DISTRIBUTION_X_LABELS = {
    "gates_between_successive_t": "Gates between successive T/TDG gates",
    "first_gate_after_t": "First gate after T/TDG",
    "second_gate_after_t": "Second gate after T/TDG",
    "first_two_gates_after_t_pair": "Ordered first-two-gate pair after T/TDG",
}


CRITICAL_PATH_T_DISTRIBUTION_Y_LABEL = "Percentage"


NO_PAULI_PLOT_TITLE_SUFFIX = " (No Pauli)"


def configure_matplotlib_for_ieee_hpca() -> None:

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
            "legend.title_fontsize": IEEE_HPCA_TICK_FONT_SIZE,
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



def ieee_hpca_subplots(height_inches: float):
    return plt.subplots(
        figsize=(IEEE_HPCA_TEXT_WIDTH_IN, height_inches),
        dpi=IEEE_HPCA_DPI,
        constrained_layout=True,
    )



def save_ieee_hpca_figure(fig, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=IEEE_HPCA_DPI)



def style_ieee_hpca_axis(ax, *, y_grid: bool = True) -> None:
    ax.tick_params(axis="both", which="both", length=2.2, width=0.5, pad=1.5)
    for spine in ax.spines.values():
        spine.set_linewidth(0.6)
    if y_grid:
        ax.grid(True, axis="y", linestyle="--", linewidth=0.35, alpha=0.35)



def odd_distance_ticks(distances) -> tuple[int, ...]:
    plotted_distances = tuple(distances)
    min_tick = DEFAULT_DISTANCE_TICKS[0]
    max_tick = DEFAULT_DISTANCE_TICKS[-1]
    if plotted_distances:
        max_tick = max(max_tick, max(plotted_distances))
    if min_tick % 2 == 0:
        min_tick = max(1, min_tick - 1)
    if max_tick % 2 == 0:
        max_tick += 1
    return tuple(range(min_tick, max_tick + 1, 2))



def set_code_distance_axis(ax, distances) -> None:
    ticks = odd_distance_ticks(distances)
    ax.set_xticks(ticks)
    ax.set_xlim(ticks[0] - 0.75, ticks[-1] + 0.75)



def ieee_hpca_tick_font_size(item_count: int) -> float:
    return max(
        IEEE_HPCA_MIN_TICK_FONT_SIZE,
        min(IEEE_HPCA_TICK_FONT_SIZE, 420 / max(item_count, 1)),
    )



def ieee_hpca_circuit_label(circuit: str) -> str:
    label = re.sub(r"_reordered$", "", circuit)
    for prefix, replacement in (
        ("large_", "L-"),
        ("medium_", "M-"),
        ("small_", "S-"),
    ):
        if label.startswith(prefix):
            label = replacement + label[len(prefix):]
            break
    return label.replace("_", " ")



configure_matplotlib_for_ieee_hpca()


def format_axis_percentage(value: float) -> str:
    return f"{value:.1f}%"



def code_volume_rows_by_scenario(rows: list[dict]) -> list[tuple[str, list[dict]]]:
    scenarios = list(dict.fromkeys(str(row["scenario"]) for row in rows))
    return [
        (
            scenario,
            sorted(
                [row for row in rows if row["scenario"] == scenario],
                key=lambda row: int(row["distance"]),
            ),
        )
        for scenario in scenarios
    ]



def write_code_volume_idle_ratio_by_distance_plot(
    rows: list[dict],
    path: Path,
    decoder_bucket: int,
) -> None:

    if not rows:
        return

    rows_by_scenario = code_volume_rows_by_scenario(rows)
    _scenario, scenario_rows = rows_by_scenario[0]
    distances = [int(row["distance"]) for row in scenario_rows]
    ratio_values = [float(row["geomean_ratio_percent"]) for row in scenario_rows]
    decoder_tw_values = [float(row["decoder_tw"]) for row in scenario_rows]

    fig, ax = ieee_hpca_subplots(IEEE_HPCA_STANDARD_FIGURE_HEIGHT_IN)
    decoder_ax = ax.twinx()
    bars = ax.bar(
        distances,
        ratio_values,
        width=0.9,
        label="Geomean idle ratio",
        color=CODE_VOLUME_RATIO_COLOR,
        edgecolor="0.25",
        linewidth=0.35,
    )
    decoder_line, = decoder_ax.plot(
        distances,
        decoder_tw_values,
        label=r"Decoder $\tau_w$",
        color=PLOT_COLORS[1],
        marker="o",
        markerfacecolor="white",
        linestyle="-",
        linewidth=1.1,
        markersize=3.2,
    )

    ax.set_xlabel("Code distance")
    ax.set_ylabel("Geomean idle ratio (%)")
    ax.yaxis.set_major_formatter(
        FuncFormatter(lambda value, _position: format_axis_percentage(value))
    )
    decoder_ax.set_ylabel(r"Decoder $\tau_w$ ($\mu$s)")
    ax.set_ylim(0, min(100, max(ratio_values, default=0) * 1.12 or 1))
    decoder_ax.set_ylim(0, max(decoder_tw_values, default=0) * 1.12 or 1)
    set_code_distance_axis(ax, distances)
    style_ieee_hpca_axis(ax)
    style_ieee_hpca_axis(decoder_ax, y_grid=False)
    ax.legend(
        handles=[bars, decoder_line],
        loc="upper right",
        frameon=False,
        handlelength=1.8,
    )
    save_ieee_hpca_figure(fig, path)
    plt.close(fig)



def sorted_distribution_items(counter: Counter, distribution: str) -> list[tuple[object, int]]:
    if distribution == "gates_between_successive_t":
        return sorted(counter.items())
    return sorted(counter.items(), key=lambda item: (-item[1], str(item[0])))



def critical_path_t_distribution_plot_title(
    distribution: str,
    circuit: str | None = None,
) -> str:
    title = CRITICAL_PATH_T_DISTRIBUTION_TITLES[distribution]
    if circuit is None:
        return title
    return f"{ieee_hpca_circuit_label(circuit)}: {title}"



def format_critical_path_pair_label(label: str) -> str:
    if " -> " not in label:
        return label
    return " -> ".join(
        r"$T^{\dagger}$" if token == "tdg" else r"$T$" if token == "t" else token
        for token in label.split(" -> ")
    )



def write_critical_path_t_distribution_plot(
    counter: Counter,
    path: Path,
    distribution: str,
    title: str | None = None,
    y_label: str | None = None,
) -> None:

    items = sorted_distribution_items(counter, distribution)
    if not items:
        return

    labels = [
        format_critical_path_pair_label(str(value))
        if distribution == "first_two_gates_after_t_pair"
        else str(value)
        for value, _count in items
    ]
    counts = [count for _value, count in items]
    total_count = sum(counts)
    y_values = [
        (count / total_count * 100 if total_count else 0)
        for count in counts
    ]
    fig, ax = ieee_hpca_subplots(IEEE_HPCA_COMPACT_FIGURE_HEIGHT_IN)
    ax.bar(
        range(len(labels)),
        y_values,
        color=CRITICAL_PATH_DISTRIBUTION_COLOR,
        edgecolor="0.25",
        linewidth=0.35,
    )
    plot_y_label = y_label or CRITICAL_PATH_T_DISTRIBUTION_Y_LABEL
    ax.set_xlabel(CRITICAL_PATH_T_DISTRIBUTION_X_LABELS[distribution])
    ax.set_ylabel(plot_y_label, labelpad=16)
    ax.yaxis.set_major_formatter(
        FuncFormatter(lambda value, _position: format_axis_percentage(value))
    )
    style_ieee_hpca_axis(ax)
    ax.set_ylim(0, min(100, max(y_values, default=0) * 1.18 or 1))
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(
        labels,
        rotation=90 if max(len(label) for label in labels) > 3 or len(labels) > 16 else 0,
        ha="center",
        fontsize=ieee_hpca_tick_font_size(len(labels)),
    )
    save_ieee_hpca_figure(fig, path)
    plt.close(fig)



def no_pauli_plot_title(distribution: str, circuit: str | None = None) -> str:
    return (
        critical_path_t_distribution_plot_title(distribution, circuit=circuit)
        + NO_PAULI_PLOT_TITLE_SUFFIX
    )
