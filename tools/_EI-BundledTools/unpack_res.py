import os
import platform
import subprocess

# Packed resources folder
res = "res"
# Unpacked resources folder
rext = "res_unpacked"

total_unpacked = ""

# Determine Wine executable path under Linux
wine_executable = "wine" if platform.system() == "Linux" else ""

# Change directory to the folder containing the packed resources
os.chdir(res)

# Loop over .res files
for filename in os.listdir("."):
    if filename.endswith(".res"):
        # Output filename
        out = os.path.splitext(filename)[0] + "_res"

        # Execute eipacker.exe with Wine under Linux
        if wine_executable:
            subprocess.run([wine_executable, "../bin/eipacker.exe", filename])
        else:
            # Execute eipacker.exe directly under Windows
            subprocess.run(["../bin/eipacker.exe", filename], shell=True)

        print(f"Done unpacking {filename} -> {out}")

        # Use rsync to move extracted files to desired folder (under Linux)
        if platform.system() == "Linux":
            subprocess.run(["rsync", "-r", "--remove-source-files", f"./{out}", f"../{rext}/"])
            print(f"Rsync completed on {out}")
        else:
            # Use robocopy to move extracted files to desired folder (under Windows)
            subprocess.run(["robocopy", f"./{out}", f"../{rext}/", "/MOVE"], shell=True)
            print(f"Robocopy completed on {out}")

        # Delete empty directories
        subprocess.run(["find", ".", "-depth", "-type", "d", "-empty", "-delete"])
        print("Empty directories deleted")

        # Add processed file to the list of processed files
        total_unpacked += f" {filename}"

print("DONE UNPACKING ASSETS")
print(f"Processed the following file(s): {total_unpacked}")
