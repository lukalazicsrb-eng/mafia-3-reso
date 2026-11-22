import importlib.util
import json
from enum import Enum, IntFlag, auto
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch


class ShaderStage(Enum):
    Pixel = auto()
    Compute = auto()


class ActionFlags(IntFlag):
    Drawcall = auto()
    Dispatch = auto()
    SetMarker = auto()


class TextureType(Enum):
    Texture2D = auto()
    TextureCube = auto()
    TextureCubeArray = auto()


rd = SimpleNamespace(ShaderStage=ShaderStage, ActionFlags=ActionFlags,
                     TextureType=TextureType, ResourceId=lambda: 0)
spec = importlib.util.spec_from_file_location(
    "export_reflections", Path(__file__).resolve().parents[1] / "capture" / "export_reflections.py")
exporter = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {"renderdoc": rd}):
    spec.loader.exec_module(exporter)


def chunk(name, **fields):
    def find(key):
        if key not in fields:
            return None
        return SimpleNamespace(AsResourceId=lambda: fields[key])

    return SimpleNamespace(name="ID3D11DeviceContext::" + name, FindChild=find)


def draw(event_id, chunk_indices, output=200, elements=6):
    return SimpleNamespace(eventId=event_id, flags=ActionFlags.Drawcall, numIndices=elements,
                           numInstances=1, outputs=[output], events=[
                               SimpleNamespace(chunkIndex=index, eventId=event_id if index == chunk_indices[-1] else 0)
                               for index in chunk_indices])


class ReflectionExportTests(unittest.TestCase):
    def test_context_tracking_resets_without_promoting_unrelated_draw(self):
        chunks = [chunk("PSSetShader", Context=12, pShader=8), chunk("Draw", Context=12),
                  chunk("Draw", Context=11), chunk("ClearState", Context=12), chunk("Draw", Context=12)]
        actions = [draw(10, [0, 1], elements=81), draw(11, [2]), draw(12, [3, 4])]
        targeted, fallback = exporter.select_events(actions, chunks, {"8": ShaderStage.Pixel})
        self.assertEqual([action.eventId for action, _ in targeted], [10])
        self.assertEqual([action.eventId for action, _ in fallback], [11, 12])

    def test_view_preserves_resource_and_mip_dimensions_and_uses(self):
        usage = [SimpleNamespace(eventId=i, usage=SimpleNamespace(name="PS_Resource"))
                 for i in range(1, 23)]
        descriptor = SimpleNamespace(resource=13, view=21, format=SimpleNamespace(Name=lambda: "FLOAT"),
                                     firstMip=2, numMips=3, firstSlice=6, numSlices=6,
                                     textureType=TextureType.TextureCubeArray)
        texture = SimpleNamespace(format=SimpleNamespace(Name=lambda: "TYPELESS"), width=512, height=512,
                                  mips=10, arraysize=12, msSamp=1, msQual=0, cubemap=True)
        context = {
            "controller": SimpleNamespace(GetUsage=lambda _: usage),
            "textures": {"13": texture}, "usage_cache": {},
            "resources": {"13": SimpleNamespace(name="probe", initialisationChunks=[0, 99])},
            "chunks": [chunk("CreateTexture2D")],
        }
        view = exporter.describe_view(descriptor, 3, "srv", 12, context)
        self.assertEqual((view["width"], view["view_width"], view["samples"]), (512, 128, 1))
        self.assertEqual((view["format"], view["resource_format"]), ("FLOAT", "TYPELESS"))
        self.assertTrue(view["is_cube"])
        self.assertEqual(view["usage_before"][0]["event_id"], 6)
        self.assertEqual(view["usage_after"][-1]["event_id"], 18)
        self.assertEqual(view["creation_chunks"], ["ID3D11DeviceContext::CreateTexture2D"])

    def test_failed_replay_emits_error_report(self):
        with TemporaryDirectory() as directory:
            capture = Path(directory) / "sample.rdc"
            report = Path(directory) / "report.json"
            env = {"MAFIA_REFLECTION_CAPTURE": str(capture), "MAFIA_REFLECTION_REPORT": str(report)}
            with patch.dict(exporter.os.environ, env), \
                    patch.object(exporter, "read_capture", side_effect=RuntimeError("replay failed")):
                self.assertFalse(exporter.main())
            self.assertEqual(json.loads(report.read_text(encoding="utf-8")), {
                "schema_version": 1, "capture": "sample.rdc", "status": "error",
                "error": "replay failed", "passes": [],
            })

    def test_report_cannot_replace_capture(self):
        with TemporaryDirectory() as directory:
            capture = str(Path(directory) / "sample.rdc")
            env = {"MAFIA_REFLECTION_CAPTURE": capture, "MAFIA_REFLECTION_REPORT": capture}
            with patch.dict(exporter.os.environ, env), self.assertRaises(ValueError):
                exporter.main()


if __name__ == "__main__":
    unittest.main()
