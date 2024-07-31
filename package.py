name = "maya_usd"

version = "0.28.0.hh.1.0.1"

authors = [
    "Autodesk",
]

description = """Maya USD plugin"""

with scope("config") as c:
    import os

    c.release_packages_path = os.environ["HH_REZ_REPO_RELEASE_EXT"]

requires = []

private_build_requires = [
    "PyOpenGL",
    "Jinja2",
    "PySide6",
    "PyYAML",
]

variants = [
    ["maya-2025.1", "usd-23.11", "python-3.11"],
]


def commands():
    env.REZ_MAYA_USD_ROOT = "{root}"
    env.MAYA_USD_ROOT = "{root}"
    env.MAYA_USD_LOCATION = "{root}"


def post_commands():
    # NOTE: We prepend (so Maya finds it first) here (post_commands) so the Maya's
    # package (hh_rez_maya) looses the race.
    env.MAYA_MODULE_PATH.prepend("{root}")


uuid = "repository.maya-usd"
