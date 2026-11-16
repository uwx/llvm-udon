import os
import lit.formats

config.name = "llvm-udon"
# The internal shell: the fixtures use POSIX-ish pipelines and Windows paths,
# and an external shell would mangle both.
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ll"]
# `Inputs/` holds hand-written fixtures other tests compile; it is not a test.
config.excludes = ["Inputs"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))
config.test_exec_root = os.path.join(config.llvm_obj_root, "test")

# The fixtures spell the tools by name, so the substitutions replace those
# names rather than inventing `%`-prefixed ones.
config.substitutions.append(("llvm-udon ", config.llvm_udon + " "))
config.substitutions.append(("udon-vm ", config.udon_vm + " "))
config.substitutions.append(("FileCheck ", config.filecheck + " "))

config.environment["PATH"] = os.pathsep.join(
    [config.llvm_tools_dir, os.environ.get("PATH", "")])
