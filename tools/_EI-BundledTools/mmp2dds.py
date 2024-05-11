import os
import platform
import subprocess

# Resource folders
dds = "dds"
mmp = "mmp"

total_dds = ""

# Determine Wine executable path under Linux
wine_executable = "wine" if platform.system() == "Linux" else ""

# Change directory to the folder containing the MMP files
os.chdir(mmp)

# Loop over *.mmp files
for filename in os.listdir("."):
    if filename.endswith(".mmp"):
        # Output filename
        out = filename[:-3] + "dds"

        # Execute MMPS.exe with Wine under Linux
        if wine_executable:
            subprocess.run([wine_executable, "../bin/MMPS.exe", filename])
        else:
            # Execute MMPS.exe directly under Windows
            subprocess.run(["../bin/MMPS.exe", filename], shell=True)

        # Move converted file to desired folder
        subprocess.run(["mv", "-fv", out, f"../{dds}/"])
        print(f"Processed {filename} -> {out}")

        # Add processed file to the list of processed files
        total_dds += f" {filename}"

print("DONE CONVERTING FILES TO MMP")
print(f"Processed the following file(s): {total_dds}")
