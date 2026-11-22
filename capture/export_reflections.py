"""Export observed Mafia III reflection pass metadata from a RenderDoc 1.47 capture."""

from collections import Counter
from hashlib import sha256
import json
import os
from pathlib import Path
import tempfile
from time import monotonic
from zlib import crc32

import renderdoc as rd


# Public technical reference, not a whitelist for changing GPU resources.
REFLECTION_SHADERS = {
    0x3117C877: "Cubemap-array probe and sky sampling",
    0x685D5D10: "SSR candidate",
    0x7E83AA58: "SSR candidate",
    0xA2C3D0F2: "Possible SSR pass",
}
TARGETED_EVENT_LIMIT = 160
FALLBACK_EVENT_LIMIT = 64
REPLAY_BUDGET_SECONDS = 105
USAGE_LIMIT = 6


def actions_recursive(actions):
    for action in actions:
        yield action
        yield from actions_recursive(action.children)


def known_shader_resources(controller):
    shaders = {}
    for resource in controller.GetResources():
        if resource.type != rd.ResourceType.Shader:
            continue
        for entry in controller.GetShaderEntryPoints(resource.resourceId):
            if entry.stage not in (rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
                continue
            shader = controller.GetShader(rd.ResourceId(), resource.resourceId, entry)
            if shader is not None and shader.rawBytes and crc32(bytes(shader.rawBytes)) in REFLECTION_SHADERS:
                shaders[str(resource.resourceId)] = entry.stage
    return shaders


def chunk_for(chunks, event):
    index = event.chunkIndex
    return chunks[index] if 0 <= index < len(chunks) else None


def chunk_resource(chunk, member):
    child = chunk.FindChild(member)
    return str(child.AsResourceId()) if child is not None else None


def stage_for(action):
    if action.flags & rd.ActionFlags.Drawcall:
        return rd.ShaderStage.Pixel
    if action.flags & rd.ActionFlags.Dispatch:
        return rd.ShaderStage.Compute
    return None


def select_events(actions, chunks, known_shaders):
    """Track structured shader binds without GPU-replaying every draw."""
    binds = {}
    targeted = []
    fallback = []
    output_counts = Counter()
    null_resource = str(rd.ResourceId())
    binding_methods = {"PSSetShader": rd.ShaderStage.Pixel, "CSSetShader": rd.ShaderStage.Compute}
    reset_methods = {"ClearState", "ExecuteCommandList", "SwapDeviceContextState"}
    for action in actions:
        stage = stage_for(action)
        context = None
        for event in action.events:
            chunk = chunk_for(chunks, event)
            if chunk is None:
                continue
            method = chunk.name.rsplit("::", 1)[-1]
            if method in binding_methods:
                binding_context = chunk_resource(chunk, "Context")
                shader_id = chunk_resource(chunk, "pShader")
                if binding_context is not None:
                    binds[binding_context, binding_methods[method]] = shader_id
            elif method in reset_methods:
                binding_context = chunk_resource(chunk, "Context")
                for shader_stage in binding_methods.values():
                    binds.pop((binding_context, shader_stage), None)
            if stage is not None and event.eventId == action.eventId:
                context = chunk_resource(chunk, "Context")

        if stage is None:
            continue
        shader_id = binds.get((context, stage))
        if shader_id is not None and known_shaders.get(shader_id) == stage:
            targeted.append((action, stage))
        elif stage == rd.ShaderStage.Pixel and action.numIndices in (3, 4, 6):
            color = str(action.outputs[0]) if action.outputs else null_resource
            if color != null_resource and output_counts[color] < 2:
                fallback.append((action, stage))
                output_counts[color] += 1
    return targeted, fallback


def use_examples(controller, resource, event_id, usage_cache):
    resource_id = str(resource)
    if resource_id not in usage_cache:
        usage_cache[resource_id] = sorted(controller.GetUsage(resource), key=lambda entry: entry.eventId)
    usage = usage_cache[resource_id]
    before = [entry for entry in usage if entry.eventId < event_id][-USAGE_LIMIT:]
    after = [entry for entry in usage if entry.eventId > event_id][:USAGE_LIMIT]

    def describe(entries):
        return [{"event_id": entry.eventId, "usage": entry.usage.name} for entry in entries]

    return describe(before), describe(after)


def describe_view(descriptor, slot, kind, event_id, context):
    if descriptor is None or descriptor.resource == rd.ResourceId():
        return None
    resource_id = str(descriptor.resource)
    texture = context["textures"].get(resource_id)
    if texture is None:
        return None
    controller = context["controller"]
    before, after = use_examples(controller, descriptor.resource, event_id, context["usage_cache"])
    resource = context["resources"].get(resource_id)
    chunks = context["chunks"]
    creation = [chunks[i].name for i in resource.initialisationChunks if 0 <= i < len(chunks)] if resource else []
    return {
        "slot": slot,
        "kind": kind,
        "resource_id": resource_id,
        "view_id": str(descriptor.view),
        "resource_name": resource.name if resource else None,
        "format": descriptor.format.Name(),
        "resource_format": texture.format.Name(),
        "width": texture.width,
        "height": texture.height,
        "view_width": max(1, texture.width >> descriptor.firstMip),
        "view_height": max(1, texture.height >> descriptor.firstMip),
        "mips": texture.mips,
        "first_mip": descriptor.firstMip,
        "num_mips": descriptor.numMips,
        "array_size": texture.arraysize,
        "first_slice": descriptor.firstSlice,
        "num_slices": descriptor.numSlices,
        "samples": texture.msSamp,
        "sample_quality": texture.msQual,
        "is_cube": texture.cubemap,
        "texture_type": descriptor.textureType.name,
        "creation_chunks": creation[:12],
        "usage_before": before,
        "usage_after": after,
    }


def shader_views(descriptors, reflection_bindings, kind, event_id, context):
    views = []
    for binding in descriptors:
        access = binding.access
        if access.index >= len(reflection_bindings):
            continue
        slot = reflection_bindings[access.index].fixedBindNumber + access.arrayElement
        view = describe_view(binding.descriptor, slot, kind, event_id, context)
        if view is not None:
            views.append(view)
    return sorted(views, key=lambda view: view["slot"])


def inspect_event(action, stage, context):
    event_id = action.eventId
    controller = context["controller"]
    controller.SetFrameEvent(event_id, False)
    pipe = controller.GetPipelineState()
    shader = pipe.GetShaderReflection(stage)
    if shader is None or not shader.rawBytes:
        return None
    bytecode = bytes(shader.rawBytes)
    checksum = crc32(bytecode)
    name = REFLECTION_SHADERS.get(checksum)
    if not name and not any(
        resource.textureType in (rd.TextureType.TextureCube, rd.TextureType.TextureCubeArray)
        for resource in shader.readOnlyResources
    ):
        return None

    reads = shader_views(pipe.GetReadOnlyResources(stage, True), shader.readOnlyResources,
                         "srv", event_id, context)
    if not name and not any(item["is_cube"] for item in reads):
        return None
    outputs = shader_views(pipe.GetReadWriteResources(stage, True), shader.readWriteResources,
                           "uav", event_id, context)
    viewport = None
    depth = None
    topology = None
    if stage == rd.ShaderStage.Pixel:
        for slot, descriptor in enumerate(pipe.GetOutputTargets()):
            view = describe_view(descriptor, slot, "rtv", event_id, context)
            if view is not None:
                outputs.append(view)
        depth = describe_view(pipe.GetDepthTarget(), 0, "dsv", event_id, context)
        view = pipe.GetViewport(0)
        viewport = {"x": view.x, "y": view.y, "width": view.width, "height": view.height}
        topology = pipe.GetPrimitiveTopology().name
    return {
        "event_id": event_id,
        "classification": "reflection_reference" if name else "candidate_only",
        "stage": stage.name,
        "shader_crc32": f"{checksum:08X}",
        "shader_sha256": sha256(bytecode).hexdigest(),
        "shader_bytes": len(bytecode),
        "reference_name": name,
        "viewport": viewport,
        "topology": topology,
        "draw_elements": action.numIndices if stage == rd.ShaderStage.Pixel else None,
        "draw_instances": action.numInstances if stage == rd.ShaderStage.Pixel else None,
        "reads": reads,
        "outputs": outputs,
        "depth": depth,
    }


def analyze(controller):
    context = {
        "controller": controller,
        "chunks": controller.GetStructuredFile().chunks,
        "textures": {str(item.resourceId): item for item in controller.GetTextures()},
        "resources": {str(item.resourceId): item for item in controller.GetResources()},
        "usage_cache": {},
    }
    actions = sorted(actions_recursive(controller.GetRootActions()), key=lambda action: action.eventId)
    known_shaders = known_shader_resources(controller)
    targeted, fallback = select_events(actions, context["chunks"], known_shaders)
    passes = []
    replayed = 0
    deadline = monotonic() + REPLAY_BUDGET_SECONDS
    for action, stage in targeted[:TARGETED_EVENT_LIMIT] + fallback[:FALLBACK_EVENT_LIMIT]:
        if replayed and monotonic() >= deadline:
            break
        report = inspect_event(action, stage, context)
        if report is not None:
            passes.append(report)
        replayed += 1
    passes.sort(key=lambda item: item["event_id"])
    return {
        "coverage": {
            "actions": len(actions),
            "reference_shaders_in_capture": len(known_shaders),
            "targeted_event_candidates": len(targeted),
            "fallback_event_candidates": len(fallback),
            "replayed_events": replayed,
            "selection_is_bounded": True,
        },
        "passes": passes,
    }


def read_capture(path):
    capture = rd.OpenCaptureFile()
    try:
        result = capture.OpenFile(str(path), "", None)
        if result.code != rd.ResultCode.Succeeded:
            raise RuntimeError(result.Message())
        if capture.DriverName() != "D3D11":
            raise RuntimeError(f"Expected D3D11 capture, got {capture.DriverName()}.")
        result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
        if result.code != rd.ResultCode.Succeeded:
            raise RuntimeError(result.Message())
        try:
            return analyze(controller)
        finally:
            controller.Shutdown()
    finally:
        capture.Shutdown()


def write_atomic(path, report):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=path.parent,
                                     suffix=".json.tmp", delete=False) as stream:
        temporary_path = Path(stream.name)
        try:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        except BaseException:
            stream.close()
            temporary_path.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def main():
    capture = Path(os.environ["MAFIA_REFLECTION_CAPTURE"])
    output = Path(os.environ["MAFIA_REFLECTION_REPORT"])
    if capture.resolve() == output.resolve():
        raise ValueError("Report path must differ from the capture path.")
    report = {"schema_version": 1, "capture": capture.name, "status": "ok"}
    try:
        report.update(read_capture(capture))
    except Exception as error:
        report.update({"status": "error", "error": str(error), "passes": []})
    write_atomic(output, report)
    return report["status"] == "ok"


if __name__ == "__main__":
    main()
    # qrenderdoc --python is an interactive GUI unless the script exits it.
    raise SystemExit
