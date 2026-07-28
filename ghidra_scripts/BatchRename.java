// BatchRename.java
// Ghidra script — batch rename functions and labels from a CSV file.
//
// CSV format (header row is skipped automatically):
//
//   address,name,comment
//   0x001a3c40,battle_player_update_state,updates player state each frame
//   0x0023ff10,char_goku_kamehameha_startup,
//   001a3c40,battle_player_update_state    <- 0x prefix is optional
//
// Columns:
//   address  (required) - hex address of the function or label
//   name     (required) - new name to apply
//   comment  (optional) - plate comment to attach above the function
//
// Installation:
//   Copy this file to: <Ghidra>/Ghidra/Features/Base/ghidra_scripts/
//   Then in Ghidra: Window > Script Manager > find BatchRename > Run
//
// @category PS2-Recomp
// @author SDBZ Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

public class BatchRename extends GhidraScript {

    @Override
    public void run() throws Exception {

        File csvFile = askFile("Select rename CSV", "Open");
        if (csvFile == null) {
            popup("No file selected. Aborting.");
            return;
        }

        int renamed  = 0;
        int labeled  = 0;
        int skipped  = 0;
        List<String> errorLog = new ArrayList<>();

        BufferedReader reader = new BufferedReader(new FileReader(csvFile));
        String line;
        int lineNum = 0;

        while ((line = reader.readLine()) != null) {
            lineNum++;
            line = line.trim();

            // Skip blank lines
            if (line.isEmpty()) {
                skipped++;
                continue;
            }

            String[] parts = line.split(",", -1);

            if (parts.length < 2) {
                skipped++;
                continue;
            }

            String addrStr = parts[0].trim();
            String name    = parts[1].trim();
            String comment = parts.length > 2 ? parts[2].trim() : "";

            // Skip header row
            if (addrStr.equalsIgnoreCase("address") || addrStr.equalsIgnoreCase("addr")) {
                continue;
            }

            if (addrStr.isEmpty() || name.isEmpty()) {
                skipped++;
                continue;
            }

            try {
                // Parse hex address with or without 0x prefix
                String cleanAddr = addrStr.toLowerCase().replace("0x", "");
                long addrLong = Long.parseLong(cleanAddr, 16);
                Address addr = toAddr(addrLong);

                Function func = getFunctionAt(addr);

                if (func != null) {
                    String oldName = func.getName();
                    func.setName(name, SourceType.USER_DEFINED);
                    if (!comment.isEmpty()) {
                        setPlateComment(addr, comment);
                    }
                    println(String.format("[RENAMED]  line %4d | %s | %s -> %s", lineNum, addrStr, oldName, name));
                    renamed++;
                } else {
                    createLabel(addr, name, true, SourceType.USER_DEFINED);
                    if (!comment.isEmpty()) {
                        setPlateComment(addr, comment);
                    }
                    println(String.format("[LABEL]    line %4d | %s | %s", lineNum, addrStr, name));
                    labeled++;
                }

            } catch (Exception e) {
                String msg = String.format("line %4d | %s | ERROR: %s", lineNum, addrStr, e.getMessage());
                errorLog.add(msg);
                println("[ERROR]    " + msg);
            }
        }

        reader.close();

        String summary = "=== Batch Rename Complete ===\n"
            + "Functions renamed : " + renamed  + "\n"
            + "Labels created    : " + labeled  + "\n"
            + "Rows skipped      : " + skipped  + "\n"
            + "Errors            : " + errorLog.size();

        if (!errorLog.isEmpty()) {
            summary += "\n\nErrors:\n" + String.join("\n", errorLog);
        }

        println("\n" + summary);
        popup(summary);
    }
}
