name = "maya_usd"

version = "0.28.0.hh.1.0.0"

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

    env.MAYA_MODULE_PATH.append("{root}")


uuid = "repository.maya-usd"
