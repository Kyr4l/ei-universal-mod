import os
import platform
import subprocess

# Resource folders
dds = "dds"
mmp = "mmp"

total_mmp = ""

# Determine Wine executable path under Linux
wine_executable = "wine" if platform.system() == "Linux" else ""

# Change directory to the folder containing the DDS files
os.chdir(dds)

# Loop over *.dds files
for filename in os.listdir("."):
    if filename.endswith(".dds"):
        # Output filename
        out = filename[:-3] + "mmp"

        # Execute MMPS.exe with Wine under Linux
        if wine_executable:
            subprocess.run([wine_executable, "../bin/MMPS.exe", filename])
        else:
            # Execute MMPS.exe directly under Windows
            subprocess.run(["../bin/MMPS.exe", filename], shell=True)

        # Move converted file to desired folder
        subprocess.run(["mv", "-fv", out, f"../{mmp}/"])
        print(f"Processed {filename} -> {out}")

        # Add processed file to the list of processed files
        total_mmp += f" {filename}"

print("DONE CONVERTING FILES TO MMP")
print(f"Processed the following file(s): {total_mmp}")
