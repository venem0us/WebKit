# Use the original header path so pragma once works without symlink privileges.
# Remove first: a previous build may have made the destination a hard link.
file(REMOVE "${DESTINATION_HEADER}")
file(WRITE "${DESTINATION_HEADER}" "#include \"${SOURCE_HEADER}\"\n")
