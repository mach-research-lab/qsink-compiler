#!/usr/bin/env python3
"""Offline RTLS schedule auditor and visualizer."""

from __future__ import annotations

import argparse
import html
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


JSON_ORIENTATIONS = ("Top", "Bottom", "Left", "Right")
EDGE_TYPES = {
    "None",
    "Solid",
    "SolidStiched",
    "Dashed",
    "DashedStiched",
    "AncillaJoin",
}
PATCH_TYPES = {"Qubit", "DistillationQubit", "Ancilla"}
ACTIVITY_TYPES = {"Unitary", "Measurement"}
SOURCE_OPERATIONS = {"TStar", "TStardg"}
CONDITIONAL_S_OPERATIONS = {"CondS", "CondSdg"}
OP_ID_RE = re.compile(r"^op_[0-9]+$")

METADATA_FIELDS = (
    "operation",
    "operation-id",
    "conditional-correction-id",
    "conditional-correction-operation-id",
    "conditional-correction-latency",
    "conditional-correction-anchor-operation-id",
    "conditional-correction-post-anchor-operation-id",
    "conditional",
    "correction-for-operation-id",
    "condition-source-slice",
    "correction-anchor-operation-id",
    "correction-post-anchor-operation-id",
)

DEFAULT_HTML_CELL_SIZE = 60


class RtlsValidationError(ValueError):
    pass


@dataclass(frozen=True)
class ActiveCell:
    slice_index: int
    row: int
    col: int
    patch_type: str
    text: str
    edges: Dict[str, str]
    activity_type: str
    operation: str
    operation_id: str
    conditional: bool
    activity: Dict[str, Any]


@dataclass
class OperationSummary:
    operation_id: str
    operation: str
    first_slice: int
    last_slice: int
    cells: List[ActiveCell]
    metadata: Dict[str, Any]


@dataclass(frozen=True)
class CorrectionChain:
    source_id: str
    source_operation: str
    correction_id: str
    correction_operation: str
    correction_slice: int
    latency: Any
    anchor_id: Optional[str]
    post_anchor_id: Optional[str]
    status: str
    detail: str


@dataclass(frozen=True)
class AuxiliaryFrameCorrection:
    source_id: str
    correction_id: str
    correction_operation: str
    correction_slice: int
    anchor_id: str
    status: str
    detail: str


@dataclass
class AuditResult:
    schedule: List[Any]
    rows: int
    cols: int
    active_cells: List[ActiveCell]
    operations: Dict[str, OperationSummary]
    correction_chains: List[CorrectionChain]
    auxiliary_frame_corrections: List[AuxiliaryFrameCorrection]
    warnings: List[str]


def load_schedule(path: Path) -> List[Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list):
        raise RtlsValidationError("RTLS schedule must be a JSON list")
    return data


def audit_schedule(schedule: List[Any], require_complete_links: bool = False) -> AuditResult:
    if not isinstance(schedule, list):
        raise RtlsValidationError("RTLS schedule must be a JSON list")
    if not schedule:
        raise RtlsValidationError("RTLS schedule must contain at least one slice")

    expected_dims: Optional[Tuple[int, int]] = None
    active_cells: List[ActiveCell] = []
    operations: Dict[str, OperationSummary] = {}
    metadata_by_id: Dict[str, Dict[str, Any]] = {}
    correction_slices: Dict[int, List[str]] = {}
    source_by_id: Dict[str, Dict[str, Any]] = {}
    correction_by_id: Dict[str, Dict[str, Any]] = {}
    auxiliary_pauli_by_id: Dict[str, Dict[str, Any]] = {}
    active_operation_ids = set()
    frame_update_operation_ids = set()
    conditional_non_pauli_frame_ids = set()
    warnings: List[str] = []

    def record_operation(
        *,
        slice_index: int,
        row_index: int,
        col_index: int,
        cell: Dict[str, Any],
        activity: Dict[str, Any],
        activity_type: str,
        physical_active: bool,
        context: str,
    ) -> None:
        operation = require_string(activity, "operation", context)
        operation_id = require_string(activity, "operation-id", context)
        if not OP_ID_RE.match(operation_id):
            raise RtlsValidationError(f"Invalid RTLS operation-id {operation_id!r}")

        conditional = activity.get("conditional", False)
        if not isinstance(conditional, bool):
            raise RtlsValidationError(f"activity.conditional must be boolean for {operation_id}")
        frame_update = activity_type == "FrameUpdate"
        conditional_frame = frame_update and conditional
        auxiliary_pauli = conditional_frame and operation in {"X", "Y", "Z"}
        if conditional_frame and not auxiliary_pauli:
            conditional_non_pauli_frame_ids.add(operation_id)
        if auxiliary_pauli:
            if operation_id in active_operation_ids:
                raise RtlsValidationError(
                    f"RTLS frame-update operation-id {operation_id} collides with an active operation"
                )
            frame_update_operation_ids.add(operation_id)
        elif not frame_update:
            if operation_id in frame_update_operation_ids:
                raise RtlsValidationError(
                    f"RTLS active operation-id {operation_id} collides with a frame update"
                )
            active_operation_ids.add(operation_id)

        metadata = metadata_signature(activity)
        previous_metadata = metadata_by_id.setdefault(operation_id, metadata)
        if previous_metadata != metadata:
            raise RtlsValidationError(f"RTLS operation-id {operation_id} has inconsistent metadata")

        active = ActiveCell(
            slice_index=slice_index,
            row=row_index,
            col=col_index,
            patch_type=cell["patch_type"],
            text=cell.get("text", ""),
            edges=dict(cell["edges"]),
            activity_type=activity_type,
            operation=operation,
            operation_id=operation_id,
            conditional=conditional,
            activity=dict(activity),
        )
        if physical_active:
            active_cells.append(active)

        if operation_id in operations:
            summary = operations[operation_id]
            if summary.operation != operation:
                raise RtlsValidationError(
                    f"RTLS operation-id {operation_id} reused for {summary.operation} and {operation}"
                )
            summary.last_slice = max(summary.last_slice, slice_index)
            summary.cells.append(active)
        else:
            operations[operation_id] = OperationSummary(
                operation_id=operation_id,
                operation=operation,
                first_slice=slice_index,
                last_slice=slice_index,
                cells=[active],
                metadata=metadata,
            )

        if physical_active and operation in SOURCE_OPERATIONS:
            source_by_id[operation_id] = dict(activity)
        if conditional:
            if auxiliary_pauli:
                auxiliary_pauli_by_id[operation_id] = dict(activity)
            elif not frame_update:
                correction_by_id[operation_id] = dict(activity)
                correction_slices.setdefault(slice_index, []).append(operation_id)

    for slice_index, slice_grid in enumerate(schedule):
        rows, cols = validate_slice_shape(slice_grid, slice_index)
        if expected_dims is None:
            expected_dims = (rows, cols)
        elif expected_dims != (rows, cols):
            raise RtlsValidationError(
                f"Slice {slice_index} has shape {rows}x{cols}; expected {expected_dims[0]}x{expected_dims[1]}"
            )

        for row_index, row in enumerate(slice_grid):
            for col_index, cell in enumerate(row):
                validate_cell(cell, slice_index, row_index, col_index)
                if cell is None:
                    continue

                activity = cell["activity"]
                if is_active_cell(cell):
                    record_operation(
                        slice_index=slice_index,
                        row_index=row_index,
                        col_index=col_index,
                        cell=cell,
                        activity=activity,
                        activity_type=activity["activity_type"],
                        physical_active=True,
                        context=active_context(slice_index, row_index, col_index),
                    )

                for update_index, frame_update in enumerate(activity.get("frame-updates", [])):
                    record_operation(
                        slice_index=slice_index,
                        row_index=row_index,
                        col_index=col_index,
                        cell=cell,
                        activity=frame_update,
                        activity_type="FrameUpdate",
                        physical_active=False,
                        context=f"frame update ({slice_index},{row_index},{col_index})[{update_index}]",
                    )

    rows, cols = expected_dims
    for operation_id in conditional_non_pauli_frame_ids:
        if operation_id not in active_operation_ids:
            raise RtlsValidationError(
                f"RTLS standalone conditional frame update {operation_id} operation must be X, Y, or Z"
            )
    correction_chains = validate_corrections(
        schedule=schedule,
        operations=operations,
        source_by_id=source_by_id,
        correction_by_id=correction_by_id,
        correction_slices=correction_slices,
        warnings=warnings,
        require_complete_links=require_complete_links,
    )
    auxiliary_frame_corrections = validate_auxiliary_frame_corrections(
        operations=operations,
        auxiliary_pauli_by_id=auxiliary_pauli_by_id,
        active_operation_ids=active_operation_ids,
    )

    return AuditResult(
        schedule=schedule,
        rows=rows,
        cols=cols,
        active_cells=active_cells,
        operations=operations,
        correction_chains=correction_chains,
        auxiliary_frame_corrections=auxiliary_frame_corrections,
        warnings=warnings,
    )


def validate_slice_shape(slice_grid: Any, slice_index: int) -> Tuple[int, int]:
    if not isinstance(slice_grid, list):
        raise RtlsValidationError(f"Slice {slice_index} must be a 2D grid")
    if not slice_grid:
        raise RtlsValidationError(f"Slice {slice_index} must contain at least one row")
    row_length: Optional[int] = None
    for row_index, row in enumerate(slice_grid):
        if not isinstance(row, list):
            raise RtlsValidationError(f"Slice {slice_index}, row {row_index} must be a list")
        if row_length is None:
            row_length = len(row)
            if row_length == 0:
                raise RtlsValidationError(f"Slice {slice_index} rows must contain at least one cell")
        elif len(row) != row_length:
            raise RtlsValidationError(
                f"Slice {slice_index} is not rectangular: row {row_index} has length {len(row)}, expected {row_length}"
            )
    return len(slice_grid), row_length or 0


def validate_cell(cell: Any, slice_index: int, row_index: int, col_index: int) -> None:
    if cell is None:
        return
    context = f"cell ({slice_index},{row_index},{col_index})"
    if not isinstance(cell, dict):
        raise RtlsValidationError(f"RTLS {context} must be null or an object")
    if "edges" not in cell or not isinstance(cell["edges"], dict):
        raise RtlsValidationError(f"RTLS {context} missing edges object")
    for orientation in JSON_ORIENTATIONS:
        edge = cell["edges"].get(orientation)
        if edge not in EDGE_TYPES:
            raise RtlsValidationError(f"RTLS {context} has invalid {orientation} edge {edge!r}")
    patch_type = cell.get("patch_type")
    if patch_type not in PATCH_TYPES:
        raise RtlsValidationError(f"RTLS {context} has invalid patch_type {patch_type!r}")
    if "text" in cell and not isinstance(cell["text"], str):
        raise RtlsValidationError(f"RTLS {context} text must be a string")
    activity = cell.get("activity")
    if not isinstance(activity, dict):
        raise RtlsValidationError(f"RTLS {context} missing activity object")
    if "activity_type" in activity and activity["activity_type"] is not None:
        if activity["activity_type"] not in ACTIVITY_TYPES:
            raise RtlsValidationError(f"RTLS {context} has invalid activity_type {activity['activity_type']!r}")
    if "frame-updates" in activity:
        frame_updates = activity["frame-updates"]
        if not isinstance(frame_updates, list):
            raise RtlsValidationError(f"RTLS {context} activity.frame-updates must be a list")
        for update_index, frame_update in enumerate(frame_updates):
            if not isinstance(frame_update, dict):
                raise RtlsValidationError(
                    f"RTLS {context} activity.frame-updates[{update_index}] must be an object"
                )


def is_active_cell(cell: Any) -> bool:
    return (
        isinstance(cell, dict)
        and isinstance(cell.get("activity"), dict)
        and "activity_type" in cell["activity"]
        and cell["activity"]["activity_type"] is not None
    )


def active_context(slice_index: int, row_index: int, col_index: int) -> str:
    return f"active cell ({slice_index},{row_index},{col_index})"


def require_string(activity: Dict[str, Any], field: str, context: str) -> str:
    value = activity.get(field)
    if not isinstance(value, str):
        raise RtlsValidationError(f"RTLS {context} missing string activity.{field}")
    return value


def optional_string(activity: Dict[str, Any], field: str, owner: str) -> Optional[str]:
    if field not in activity:
        return None
    value = activity[field]
    if not isinstance(value, str):
        raise RtlsValidationError(f"RTLS {owner} field {field} must be a string")
    return value


def require_int(activity: Dict[str, Any], field: str, owner: str) -> int:
    value = activity.get(field)
    if not isinstance(value, int) or isinstance(value, bool):
        raise RtlsValidationError(f"RTLS {owner} field {field} must be an integer")
    return value


def validate_latency(value: Any, owner: str) -> None:
    if isinstance(value, bool):
        raise RtlsValidationError(f"RTLS {owner} latency must be 0, 'hs', or 'mz'")
    if isinstance(value, int):
        if value != 0:
            raise RtlsValidationError(f"RTLS {owner} integer latency must be 0")
        return
    if isinstance(value, str):
        if value not in {"hs", "mz"}:
            raise RtlsValidationError(f"RTLS {owner} string latency must be 'hs' or 'mz'")
        return
    raise RtlsValidationError(f"RTLS {owner} latency must be 0, 'hs', or 'mz'")


def metadata_signature(activity: Dict[str, Any]) -> Dict[str, Any]:
    return {field: activity[field] for field in METADATA_FIELDS if field in activity}


def validate_corrections(
    schedule: List[Any],
    operations: Dict[str, OperationSummary],
    source_by_id: Dict[str, Dict[str, Any]],
    correction_by_id: Dict[str, Dict[str, Any]],
    correction_slices: Dict[int, List[str]],
    warnings: List[str],
    require_complete_links: bool,
) -> List[CorrectionChain]:
    chains: List[CorrectionChain] = []

    for source_id in sorted(source_by_id, key=operation_sort_key):
        source = source_by_id[source_id]
        source_summary = operations[source_id]
        owner = f"source {source_id}"
        correction_id = require_string(source, "conditional-correction-operation-id", owner)
        correction_slice = require_int(source, "conditional-correction-id", owner)
        latency = source.get("conditional-correction-latency")
        validate_latency(latency, owner)

        if correction_slice < 0 or correction_slice >= len(schedule):
            raise RtlsValidationError(f"RTLS {owner} correction slice is outside the schedule")
        if correction_id not in correction_slices.get(correction_slice, []):
            raise RtlsValidationError(f"RTLS {owner} correction operation is not active in the referenced slice")
        if correction_id not in correction_by_id:
            raise RtlsValidationError(f"RTLS {owner} points to a non-conditional correction")

        correction = correction_by_id[correction_id]
        correction_summary = operations[correction_id]
        if correction_slice != correction_summary.first_slice:
            raise RtlsValidationError(
                f"RTLS {owner} conditional-correction-id is {correction_slice}, "
                f"expected first correction slice {correction_summary.first_slice}"
            )
        correction_for = require_string(correction, "correction-for-operation-id", f"correction {correction_id}")
        if correction_for != source_id:
            raise RtlsValidationError(f"RTLS correction {correction_id} does not point back to source {source_id}")
        condition_source_slice = require_int(correction, "condition-source-slice", f"correction {correction_id}")
        if condition_source_slice != source_summary.first_slice:
            raise RtlsValidationError(
                f"RTLS correction {correction_id} condition-source-slice is {condition_source_slice}, "
                f"expected {source_summary.first_slice}"
            )

        source_anchor = optional_string(source, "conditional-correction-anchor-operation-id", owner)
        correction_anchor = optional_string(correction, "correction-anchor-operation-id", f"correction {correction_id}")
        source_post = optional_string(source, "conditional-correction-post-anchor-operation-id", owner)
        correction_post = optional_string(correction, "correction-post-anchor-operation-id", f"correction {correction_id}")

        anchor_id = validate_optional_mirror(
            source_anchor,
            correction_anchor,
            source_id,
            correction_id,
            "anchor",
        )
        post_anchor_id = validate_optional_mirror(
            source_post,
            correction_post,
            source_id,
            correction_id,
            "post-anchor",
        )

        if anchor_id is not None:
            if anchor_id not in operations:
                raise RtlsValidationError(f"RTLS source {source_id} anchor {anchor_id} does not appear in schedule")
        if post_anchor_id is not None:
            if post_anchor_id not in operations:
                raise RtlsValidationError(
                    f"RTLS source {source_id} post-anchor {post_anchor_id} does not appear in schedule"
                )
            if not correction_summary.last_slice < operations[post_anchor_id].first_slice:
                raise RtlsValidationError(f"RTLS correction {correction_id} is not before post-anchor {post_anchor_id}")

        if latency == "mz":
            if correction_summary.operation not in CONDITIONAL_S_OPERATIONS:
                raise RtlsValidationError(f"RTLS mz source {source_id} must point to CondS or CondSdg")
            complete_links = anchor_id is not None or post_anchor_id is not None
            if require_complete_links and not complete_links:
                raise RtlsValidationError(f"RTLS mz source {source_id} missing exact anchor/post-anchor metadata")
            if complete_links:
                if anchor_id is None or post_anchor_id is None:
                    raise RtlsValidationError(f"RTLS mz source {source_id} has incomplete anchor metadata")
                if operations[anchor_id].operation != "Mz":
                    raise RtlsValidationError(f"RTLS mz source {source_id} anchor {anchor_id} is not Mz")
                if operations[post_anchor_id].operation != "Mx":
                    raise RtlsValidationError(f"RTLS mz source {source_id} post-anchor {post_anchor_id} is not Mx")
                if not operations[anchor_id].last_slice <= correction_summary.first_slice:
                    raise RtlsValidationError(f"RTLS correction {correction_id} starts before anchor {anchor_id}")
                detail = f"{anchor_id}({operations[anchor_id].last_slice}) <= {correction_id}({correction_slice}) < {post_anchor_id}({operations[post_anchor_id].first_slice})"
            else:
                warnings.append(
                    f"Source {source_id} has mz latency but no exact anchor/post-anchor metadata; "
                    "exact Mz/Mx association cannot be proven from this file alone"
                )
                detail = "legacy metadata; exact Mz/Mx association not proven"
        else:
            detail = "linked"

        chains.append(
            CorrectionChain(
                source_id=source_id,
                source_operation=source_summary.operation,
                correction_id=correction_id,
                correction_operation=correction_summary.operation,
                correction_slice=correction_slice,
                latency=latency,
                anchor_id=anchor_id,
                post_anchor_id=post_anchor_id,
                status="PASS",
                detail=detail,
            )
        )

    return chains


def validate_optional_mirror(
    source_value: Optional[str],
    correction_value: Optional[str],
    source_id: str,
    correction_id: str,
    label: str,
) -> Optional[str]:
    if source_value is None and correction_value is None:
        return None
    if source_value is None or correction_value is None:
        raise RtlsValidationError(
            f"RTLS source {source_id} and correction {correction_id} have incomplete {label} metadata"
        )
    if source_value != correction_value:
        raise RtlsValidationError(
            f"RTLS source {source_id} and correction {correction_id} disagree on {label} operation ID"
        )
    return source_value


def validate_auxiliary_frame_corrections(
    *,
    operations: Dict[str, OperationSummary],
    auxiliary_pauli_by_id: Dict[str, Dict[str, Any]],
    active_operation_ids: set[str],
) -> List[AuxiliaryFrameCorrection]:
    corrections: List[AuxiliaryFrameCorrection] = []
    for correction_id in sorted(auxiliary_pauli_by_id, key=operation_sort_key):
        activity = auxiliary_pauli_by_id[correction_id]
        summary = operations[correction_id]
        owner = f"auxiliary frame correction {correction_id}"

        source_id = require_string(activity, "correction-for-operation-id", owner)
        if source_id not in active_operation_ids:
            raise RtlsValidationError(f"RTLS {owner} source {source_id} does not appear as an active operation")
        condition_source_slice = require_int(activity, "condition-source-slice", owner)
        if condition_source_slice != operations[source_id].first_slice:
            raise RtlsValidationError(
                f"RTLS {owner} condition-source-slice is {condition_source_slice}, "
                f"expected {operations[source_id].first_slice}"
            )

        anchor_id = require_string(activity, "correction-anchor-operation-id", owner)
        if anchor_id not in active_operation_ids:
            raise RtlsValidationError(f"RTLS {owner} anchor {anchor_id} does not appear as an active operation")
        anchor = operations[anchor_id]
        if summary.first_slice < anchor.first_slice or summary.last_slice > anchor.last_slice:
            raise RtlsValidationError(
                f"RTLS {owner} slices {summary.first_slice}-{summary.last_slice} "
                f"are outside anchor {anchor_id} slices {anchor.first_slice}-{anchor.last_slice}"
            )

        corrections.append(
            AuxiliaryFrameCorrection(
                source_id=source_id,
                correction_id=correction_id,
                correction_operation=summary.operation,
                correction_slice=summary.first_slice,
                anchor_id=anchor_id,
                status="PASS",
                detail=f"same-slice frame update in {anchor_id}",
            )
        )
    return corrections


def operation_sort_key(operation_id: str) -> Tuple[int, str]:
    if operation_id.startswith("op_") and operation_id[3:].isdigit():
        return int(operation_id[3:]), operation_id
    return 10**18, operation_id


def write_html_report(
    result: AuditResult,
    output_path: Path,
    input_path: Path,
    cell_size: int = DEFAULT_HTML_CELL_SIZE,
) -> None:
    last_slice_index = len(result.schedule) - 1
    body: List[str] = []
    body.append("<!doctype html>")
    body.append("<html lang=\"en\"><head><meta charset=\"utf-8\">")
    body.append("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">")
    body.append("<title>RTLS Audit</title>")
    body.append("<style>")
    body.append(
        "*{box-sizing:border-box}"
        "body{font-family:system-ui,Arial,sans-serif;margin:0;background:#f7f8fa;color:#1f2933}"
        ".report-header{padding:14px 24px 10px;text-align:center}"
        ".report-header h1{margin:0 0 4px;font-size:clamp(18px,2.2vw,26px)}"
        ".report-header p{margin:0;font-size:14px}"
        ".viewer{height:calc(100svh - 86px);min-height:360px;padding:8px 24px 18px;display:flex;flex-direction:column;align-items:center;gap:12px}"
        ".slice-stage{width:100%;flex:1;min-height:0;display:grid;place-items:center}"
        ".slice{width:100%;height:100%;display:flex;align-items:center;justify-content:center}"
        ".slice[hidden]{display:none}"
        ".slice svg{display:block;width:auto;height:auto;max-width:100%;max-height:100%;background:white;border:1px solid #d8dee4;box-shadow:0 8px 24px rgba(31,41,51,.08)}"
        ".slice-navigator{width:min(680px,100%);display:flex;flex-direction:column;align-items:center;gap:7px}"
        ".navigator-controls{width:100%;display:grid;grid-template-columns:auto minmax(120px,1fr) auto;align-items:center;gap:12px}"
        ".navigator-controls button{min-width:92px;padding:8px 14px;border:1px solid #aeb8c2;border-radius:6px;background:white;color:#1f2933;font:inherit;font-weight:600;cursor:pointer}"
        ".navigator-controls button:hover:not(:disabled){background:#eef3f7;border-color:#788896}"
        ".navigator-controls button:focus-visible,.navigator-controls input:focus-visible{outline:3px solid rgba(9,105,218,.28);outline-offset:2px}"
        ".navigator-controls button:disabled{opacity:.45;cursor:not-allowed}"
        ".navigator-controls input{width:100%;accent-color:#0969da;cursor:pointer}"
        ".slice-position{font-size:14px;font-weight:650;font-variant-numeric:tabular-nums}"
        ".report-details{padding:28px 24px 48px;background:white;border-top:1px solid #d8dee4}"
        ".report-details-inner{width:min(1100px,100%);margin:0 auto}"
        ".table-scroll{overflow-x:auto}"
        "table{width:100%;border-collapse:collapse;margin:12px 0}"
        "td,th{border:1px solid #d8dee4;padding:6px 8px;font-size:12px;text-align:left}"
        ".warn{color:#9a6700}.pass{color:#116329;font-weight:700}code{background:#eef1f4;padding:1px 4px}"
        "@media(max-width:560px){.report-header{padding-inline:12px}.viewer{padding-inline:12px}.navigator-controls{grid-template-columns:1fr 1fr}.navigator-controls input{grid-column:1/-1;grid-row:1}.navigator-controls button{min-width:0}.report-details{padding-inline:12px}}"
    )
    body.append("</style></head><body>")
    body.append("<header class=\"report-header\">")
    body.append(f"<h1>RTLS audit: {html.escape(str(input_path))}</h1>")
    body.append(
        f"<p><span class=\"pass\">Validation PASS</span>. "
        f"Slices: {len(result.schedule)}, grid: {result.rows} x {result.cols}, "
        f"active cells: {len(result.active_cells)}, operations: {len(result.operations)}.</p>"
    )
    body.append("</header>")

    body.append("<main>")
    body.append("<section class=\"viewer\" aria-label=\"Schedule slice viewer\">")
    body.append("<div class=\"slice-stage\" id=\"slice-stage\">")
    for slice_index, slice_grid in enumerate(result.schedule):
        hidden = "" if slice_index == 0 else " hidden"
        body.append(
            f"<section class=\"slice\" data-slice-index=\"{slice_index}\"{hidden} "
            f"aria-label=\"Slice {slice_index}\">"
        )
        body.append(render_svg_slice(slice_grid, slice_index, cell_size))
        body.append("</section>")
    body.append("</div>")
    body.append("<nav class=\"slice-navigator\" aria-label=\"Slice navigation\">")
    body.append("<div class=\"navigator-controls\">")
    body.append("<button type=\"button\" id=\"previous-slice\" disabled>Previous</button>")
    body.append(
        f"<input type=\"range\" id=\"slice-slider\" min=\"0\" max=\"{last_slice_index}\" value=\"0\" "
        f"step=\"1\" aria-label=\"Slice\" aria-valuetext=\"Slice 0 of {last_slice_index}\">"
    )
    body.append("<button type=\"button\" id=\"next-slice\">Next</button>")
    body.append("</div>")
    body.append(
        f"<div class=\"slice-position\" aria-live=\"polite\">Slice "
        f"<output id=\"current-slice\" for=\"slice-slider\">0</output> of {last_slice_index}</div>"
    )
    body.append("</nav>")
    body.append("</section>")

    body.append("<section class=\"report-details\">")
    body.append("<div class=\"report-details-inner\">")
    if result.warnings:
        body.append("<h2>Warnings</h2><ul>")
        for warning in result.warnings:
            body.append(f"<li class=\"warn\">{html.escape(warning)}</li>")
        body.append("</ul>")

    body.append("<h2>Correction Chains</h2>")
    body.append("<div class=\"table-scroll\">")
    body.append("<table><tr><th>Status</th><th>Source</th><th>Correction</th><th>Latency</th><th>Anchor</th><th>Post</th><th>Detail</th></tr>")
    if not result.correction_chains:
        body.append("<tr><td colspan=\"7\">none</td></tr>")
    for chain in result.correction_chains:
        body.append(
            "<tr>"
            f"<td class=\"pass\">{html.escape(chain.status)}</td>"
            f"<td>{html.escape(chain.source_operation)} <code>{html.escape(chain.source_id)}</code></td>"
            f"<td>{html.escape(chain.correction_operation)} <code>{html.escape(chain.correction_id)}</code> @ {chain.correction_slice}</td>"
            f"<td>{html.escape(repr(chain.latency))}</td>"
            f"<td>{html.escape(chain.anchor_id or '-')}</td>"
            f"<td>{html.escape(chain.post_anchor_id or '-')}</td>"
            f"<td>{html.escape(chain.detail)}</td>"
            "</tr>"
        )
    body.append("</table>")
    body.append("</div>")

    body.append("<h2>Auxiliary Pauli Frame Corrections</h2>")
    body.append("<div class=\"table-scroll\">")
    body.append("<table><tr><th>Status</th><th>Source</th><th>Correction</th><th>Anchor</th><th>Detail</th></tr>")
    if not result.auxiliary_frame_corrections:
        body.append("<tr><td colspan=\"5\">none</td></tr>")
    for correction in result.auxiliary_frame_corrections:
        body.append(
            "<tr>"
            f"<td class=\"pass\">{html.escape(correction.status)}</td>"
            f"<td><code>{html.escape(correction.source_id)}</code></td>"
            f"<td>{html.escape(correction.correction_operation)} "
            f"<code>{html.escape(correction.correction_id)}</code> @ {correction.correction_slice}</td>"
            f"<td>{html.escape(correction.anchor_id)}</td>"
            f"<td>{html.escape(correction.detail)}</td>"
            "</tr>"
        )
    body.append("</table>")
    body.append("</div></div></section>")
    body.append("</main>")

    body.append("<script>")
    body.append(
        "const slices=Array.from(document.querySelectorAll('.slice'));"
        "const slider=document.getElementById('slice-slider');"
        "const previous=document.getElementById('previous-slice');"
        "const next=document.getElementById('next-slice');"
        "const currentOutput=document.getElementById('current-slice');"
        "const lastSlice=slices.length-1;"
        "function showSlice(value){"
        "const index=Math.max(0,Math.min(lastSlice,Number(value)));"
        "slices.forEach((slice,sliceIndex)=>{slice.hidden=sliceIndex!==index;});"
        "slider.value=String(index);"
        "slider.setAttribute('aria-valuetext',`Slice ${index} of ${lastSlice}`);"
        "currentOutput.value=String(index);"
        "previous.disabled=index===0;"
        "next.disabled=index===lastSlice;"
        "}"
        "slider.addEventListener('input',()=>showSlice(slider.value));"
        "previous.addEventListener('click',()=>showSlice(Number(slider.value)-1));"
        "next.addEventListener('click',()=>showSlice(Number(slider.value)+1));"
        "showSlice(0);"
    )
    body.append("</script>")

    body.append("</body></html>")
    output_path.write_text("\n".join(body) + "\n", encoding="utf-8")


def render_svg_slice(slice_grid: List[Any], slice_index: int, cell_size: int = DEFAULT_HTML_CELL_SIZE) -> str:
    margin = max(24, int(cell_size * 0.45))
    coord_font = max(7, int(cell_size * 0.13))
    cell_coord_font = max(6, int(cell_size * 0.11))
    text_font = max(6, int(cell_size * 0.12))
    operation_font = max(7, int(cell_size * 0.15))
    operation_id_font = max(6, int(cell_size * 0.12))
    activity_radius = max(10, int(cell_size * 0.27))
    rows = len(slice_grid)
    cols = len(slice_grid[0])
    width = cols * cell_size + margin * 2
    height = rows * cell_size + margin * 2
    parts = [
        f"<svg viewBox=\"0 0 {width} {height}\" width=\"{width}\" height=\"{height}\" "
        f"role=\"img\" aria-label=\"Slice {slice_index}\">"
    ]

    for col in range(cols):
        x = margin + col * cell_size + cell_size / 2
        parts.append(f"<text x=\"{x}\" y=\"{margin - 10}\" text-anchor=\"middle\" font-size=\"{coord_font}\">c{col}</text>")
    for row in range(rows):
        y = margin + row * cell_size + cell_size / 2
        parts.append(f"<text x=\"{margin - 16}\" y=\"{y}\" text-anchor=\"middle\" dominant-baseline=\"middle\" font-size=\"{coord_font}\">r{row}</text>")

    for row_index, row in enumerate(slice_grid):
        for col_index, cell in enumerate(row):
            x = margin + col_index * cell_size
            y = margin + row_index * cell_size
            if cell is None:
                parts.append(
                    f"<rect x=\"{x}\" y=\"{y}\" width=\"{cell_size}\" height=\"{cell_size}\" fill=\"#ffffff\" stroke=\"#d8dee4\"/>"
                )
                continue
            fill = patch_fill(cell["patch_type"])
            parts.append(
                f"<rect x=\"{x}\" y=\"{y}\" width=\"{cell_size}\" height=\"{cell_size}\" fill=\"{fill}\" stroke=\"#d8dee4\"/>"
            )
            parts.extend(render_svg_edges(x, y, cell_size, cell["edges"]))
            parts.append(
                f"<text x=\"{x + 4}\" y=\"{y + cell_coord_font + 3}\" font-size=\"{cell_coord_font}\" fill=\"#586069\">({row_index},{col_index})</text>"
            )
            if cell.get("text"):
                parts.append(
                    f"<text x=\"{x + cell_size / 2}\" y=\"{y + cell_size - 6}\" text-anchor=\"middle\" font-size=\"{text_font}\" fill=\"#30363d\">"
                    f"{html.escape(cell['text'])}</text>"
                )
            if is_active_cell(cell):
                activity = cell["activity"]
                color = "#00baff" if activity["activity_type"] == "Unitary" else "#ff0000"
                parts.append(
                    f"<circle cx=\"{x + cell_size / 2}\" cy=\"{y + cell_size / 2}\" r=\"{activity_radius}\" fill=\"{color}\" opacity=\"0.34\"/>"
                )
                label = f"{activity['operation']}\\n{activity['operation-id']}"
                parts.append(
                    f"<text x=\"{x + cell_size / 2}\" y=\"{y + cell_size / 2 - 3}\" text-anchor=\"middle\" font-size=\"{operation_font}\" fill=\"#111827\">"
                    f"{html.escape(short_operation_label(activity['operation']))}</text>"
                )
                parts.append(
                    f"<text x=\"{x + cell_size / 2}\" y=\"{y + cell_size / 2 + operation_id_font + 3}\" text-anchor=\"middle\" font-size=\"{operation_id_font}\" fill=\"#111827\">"
                    f"{html.escape(short_operation_id(activity['operation-id']))}</text>"
                )
                parts.append(f"<title>{html.escape(label)}</title>")
    parts.append("</svg>")
    return "\n".join(parts)


def render_svg_edges(x: float, y: float, size: float, edges: Dict[str, str]) -> List[str]:
    specs = {
        "Top": (x, y, x + size, y),
        "Bottom": (x, y + size, x + size, y + size),
        "Left": (x, y, x, y + size),
        "Right": (x + size, y, x + size, y + size),
    }
    lines: List[str] = []
    for orientation, coords in specs.items():
        edge = edges[orientation]
        if edge == "None":
            continue
        color, dash, width = svg_edge_style(edge)
        width = max(1, int(round(width * size / 94)))
        dash_attr = f" stroke-dasharray=\"{dash}\"" if dash else ""
        x1, y1, x2, y2 = coords
        lines.append(
            f"<line x1=\"{x1}\" y1=\"{y1}\" x2=\"{x2}\" y2=\"{y2}\" stroke=\"{color}\" stroke-width=\"{width}\"{dash_attr}/>"
        )
    return lines


def patch_fill(patch_type: str) -> str:
    return {
        "Qubit": "darkkhaki",
        "DistillationQubit": "orchid",
        "Ancilla": "aquamarine",
    }[patch_type]


def svg_edge_style(edge: str) -> Tuple[str, str, int]:
    return {
        "Solid": ("black", "", 4),
        "SolidStiched": ("#37beff", "", 4),
        "Dashed": ("black", "7 5", 4),
        "DashedStiched": ("#37beff", "7 5", 4),
        "AncillaJoin": ("aquamarine", "", 6),
    }[edge]


def short_operation_label(operation: str) -> str:
    return {
        "BellBasedCNOT": "CNOT",
        "TStar": "T*",
        "TStardg": "T*dg",
        "CondS": "cS",
        "CondSdg": "cSdg",
        "CondH": "cH",
    }.get(operation, operation)


def short_operation_id(operation_id: str) -> str:
    if operation_id.startswith("op_"):
        return operation_id[3:]
    return operation_id


def default_out_prefix(input_path: Path) -> Path:
    if input_path.suffix == ".rtls":
        return input_path.with_suffix("")
    return input_path


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate and visualize an RTLS schedule")
    parser.add_argument("rtls_file", type=Path)
    parser.add_argument("--out-prefix", type=Path, help="output path before the .html extension")
    parser.add_argument(
        "--cell-size",
        type=int,
        default=DEFAULT_HTML_CELL_SIZE,
        help=f"SVG cell size in pixels for the HTML audit (default: {DEFAULT_HTML_CELL_SIZE})",
    )
    parser.add_argument(
        "--require-complete-links",
        action="store_true",
        help="fail mz corrections that do not include exact anchor/post-anchor operation IDs",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    if args.cell_size < 32:
        raise RtlsValidationError("--cell-size must be at least 32")
    input_path = args.rtls_file
    out_prefix = args.out_prefix or default_out_prefix(input_path)
    schedule = load_schedule(input_path)
    result = audit_schedule(schedule, require_complete_links=args.require_complete_links)
    html_path = Path(str(out_prefix) + ".html")
    write_html_report(result, html_path, input_path, cell_size=args.cell_size)
    print(f"Validation PASS: wrote {html_path}")
    if result.warnings:
        for warning in result.warnings:
            print(f"WARNING: {warning}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
