# ExportSymbols.py
# Ghidra Python script to export function entry points and names to a map file.
# @author Gemini Code Assist
# @category SDBZ Recompilation
# @keybinding 
# @menupath 
# @toolbar 

import os

def export_ghidra_symbols():
    """
    Exports function entry points and names from the current Ghidra program
    to a text file in a format compatible with the SDBZ PS2 Recomp Debugger.
    """
    program = currentProgram
    if not program:
        popup("No program open. Please open a program in Ghidra.")
        return

    function_manager = program.getFunctionManager()
    if not function_manager:
        popup("Could not get Function Manager for the current program.")
        return

    output_file_path = askFile("Choose output file for symbols", "Export").getAbsolutePath()
    if not output_file_path:
        popup("No output file selected. Aborting symbol export.")
        return

    try:
        with open(output_file_path, 'w') as f:
            count = 0
            for func in function_manager.getFunctions(True): # True for forward iteration
                # Get the function's entry point address
                address = func.getEntryPoint()
                # Get the function's name
                name = func.getName()

                # Format as "Address FunctionName"
                # Use address.getOffset() for the raw offset value and format it to hex
                # Adjust padding (e.g., 8 for 32-bit addresses, 16 for 64-bit) as appropriate for PS2 EE addresses
                # PS2 EE addresses are 32-bit, so 8 hex digits should be enough.
                formatted_address = "{:08X}".format(address.getOffset()) 
                
                f.write(f"{formatted_address} {name}\n")
                count += 1
        
        popup(f"Successfully exported {count} symbols to: {output_file_path}")

    except Exception as e:
        popup(f"An error occurred during symbol export: {e}")

if __name__ == '__main__':
    export_ghidra_symbols()
