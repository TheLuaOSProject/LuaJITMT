-- Called by a build script immediately after make, with that build's exact
-- compiler/target/flag environment. Never use this to guess an old profile.
local root = assert(os.getenv("LJ_TEST_ROOT"), "LJ_TEST_ROOT is required")
package.path = root .. "/tests/lib/?.lua;" .. package.path
require("ljtest").new(root):record_build_profile()
