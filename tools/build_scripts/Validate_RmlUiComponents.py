#!/usr/bin/env python3
"""Validate the source-level contract for MacroQuest's RmlUi component gallery."""

from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESOURCE_ROOT = ROOT / "data" / "resources" / "rmlui"
MANIFEST_PATH = RESOURCE_ROOT / "mq-components.manifest.json"
GALLERY_PATH = RESOURCE_ROOT / "components.rml"
GALLERY_LUA_PATH = ROOT / "src" / "plugins" / "lua" / "lua" / "examples" / "rmlui_components.lua"
COMPONENT_STYLESHEET = RESOURCE_ROOT / "mq-components.rcss"
WINDOW_STYLESHEET = RESOURCE_ROOT / "mq-window.rcss"
DEFAULT_THEME = RESOURCE_ROOT / "mq-theme-default.rcss"


class ValidationError(Exception):
    pass


def read_text(path: Path) -> str:
    if not path.is_file():
        raise ValidationError(f"required file is missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def validate() -> list[str]:
    manifest = json.loads(read_text(MANIFEST_PATH))
    gallery_text = read_text(GALLERY_PATH)
    lua_text = read_text(GALLERY_LUA_PATH)
    component_css = read_text(COMPONENT_STYLESHEET)
    window_css = read_text(WINDOW_STYLESHEET)
    default_theme = read_text(DEFAULT_THEME)

    try:
        gallery = ET.fromstring(gallery_text)
    except ET.ParseError as error:
        raise ValidationError(f"components.rml is not well-formed XML: {error}") from error

    components = manifest.get("components")
    if not isinstance(components, list) or not components:
        raise ValidationError("manifest must contain a non-empty components array")

    names: set[str] = set()
    for component in components:
        name = component.get("name")
        if not name or name in names:
            raise ValidationError(f"component name is missing or duplicated: {name!r}")
        names.add(name)
        if component.get("status") not in {"supported", "experimental"}:
            raise ValidationError(f"component {name!r} has an invalid status")

    all_styles = re.sub(r"/\*.*?\*/", "", component_css + "\n" + window_css, flags=re.S)
    styled_classes = set(re.findall(r"\.([A-Za-z_][\w-]*)", all_styles))
    for component in components:
        for class_name in component.get("classes", []) + component.get("parts", []):
            if class_name not in styled_classes:
                raise ValidationError(
                    f"manifest class/part {class_name!r} ({component['name']}) has no shared RCSS selector"
                )

    ids: list[str] = []
    classes_in_gallery: set[str] = set()
    for element in gallery.iter():
        element_id = element.attrib.get("id")
        if element_id:
            ids.append(element_id)
        classes_in_gallery.update(element.attrib.get("class", "").split())
    duplicates = sorted({element_id for element_id in ids if ids.count(element_id) > 1})
    if duplicates:
        raise ValidationError(f"gallery contains duplicate element IDs: {', '.join(duplicates)}")

    missing_samples = [
        component["name"]
        for component in components
        if component.get("status") == "supported"
        and (
            not (component.get("classes") or component.get("parts"))
            or (component.get("classes") or component.get("parts"))[0] not in classes_in_gallery
        )
    ]
    if missing_samples:
        raise ValidationError("gallery has no sample for supported components: " + ", ".join(missing_samples))

    resource_links = 0
    for link in gallery.findall(".//link"):
        href = link.attrib.get("href", "")
        if href.startswith("mq://rmlui/"):
            target = RESOURCE_ROOT / href[len("mq://rmlui/"):]
        elif "://" in href:
            continue  # Other virtual roots are validated by the runtime path tests.
        else:
            target = GALLERY_PATH.parent / href
        try:
            target.resolve().relative_to(RESOURCE_ROOT.resolve())
        except ValueError as error:
            raise ValidationError(f"gallery resource link escapes the RmlUi resource root: {href!r}") from error
        if not target.is_file():
            raise ValidationError(f"gallery resource link does not resolve: {href!r}")
        resource_links += 1

    lua_listener_ids = set(
        re.findall(r"window:on\s*\(\s*['\"][^'\"]+['\"]\s*,\s*['\"]([^'\"]+)['\"]", lua_text)
    )
    missing_listener_ids = sorted(lua_listener_ids.difference(ids))
    if missing_listener_ids:
        raise ValidationError("Lua gallery listeners refer to missing RML IDs: " + ", ".join(missing_listener_ids))

    model_function = re.search(
        r"local\s+function\s+model\s*\(\)(.*?)\nend", lua_text, flags=re.S
    )
    if not model_function:
        raise ValidationError("could not find the gallery model() fixture in Lua")
    model_fields = set(re.findall(r"^\s{8}([A-Za-z_][\w]*)\s*=", model_function.group(1), flags=re.M))

    loop_variables: set[str] = set()
    referenced_fields: set[str] = set()
    for element in gallery.iter():
        for attribute, value in element.attrib.items():
            if attribute == "data-for":
                iteration = re.fullmatch(r"\s*([A-Za-z_][\w]*)\s*:\s*([A-Za-z_][\w.]*)\s*", value)
                if not iteration:
                    raise ValidationError(f"unsupported data-for expression in gallery: {value!r}")
                loop_variables.add(iteration.group(1))
                referenced_fields.add(iteration.group(2).split(".", 1)[0])
                continue
            if attribute in {"data-model", "data-mq-value"}:
                continue
            if not attribute.startswith("data-"):
                continue
            expression = re.sub(r"'[^']*'|\"[^\"]*\"", "", value)
            for reference in re.findall(r"\b([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)", expression):
                root_name = reference.split(".", 1)[0]
                if root_name not in loop_variables and root_name not in {"true", "false"}:
                    referenced_fields.add(root_name)

    for interpolation in re.findall(r"\{\{\s*([^}]+?)\s*\}\}", gallery_text):
        reference = re.match(r"\s*([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)", interpolation)
        if reference:
            root_name = reference.group(1).split(".", 1)[0]
            if root_name not in loop_variables:
                referenced_fields.add(root_name)

    missing_fields = sorted(referenced_fields.difference(model_fields).difference(loop_variables))
    if missing_fields:
        raise ValidationError("gallery binds fields absent from its Lua model fixture: " + ", ".join(missing_fields))

    defined_tokens = set(re.findall(r"(--mq-[\w-]+)\s*:", default_theme))
    referenced_tokens = set(re.findall(r"var\(\s*(--mq-[\w-]+)", component_css + "\n" + window_css))
    missing_tokens = sorted(referenced_tokens.difference(defined_tokens))
    if missing_tokens:
        raise ValidationError("shared component/window RCSS references undefined default theme tokens: " + ", ".join(missing_tokens))

    return [
        f"manifest components: {len(components)}",
        f"gallery IDs: {len(ids)} unique",
        f"local resource links: {resource_links}",
        f"model fields: {len(referenced_fields)} referenced, all provided",
        f"shared theme tokens: {len(referenced_tokens)} referenced, all defined",
    ]


if __name__ == "__main__":
    try:
        for line in validate():
            print(f"RmlUi component check: {line}")
    except (ValidationError, json.JSONDecodeError) as error:
        print(f"RmlUi component check FAILED: {error}", file=sys.stderr)
        sys.exit(1)
