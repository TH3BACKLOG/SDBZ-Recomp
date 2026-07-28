// AnalyzeFunction.java
//
// Ghidra script — decompiles the function at the cursor, sends the MIPS assembly
// and decompiled C to a local Ollama model, and writes the analysis as a plate comment.
//
// Requirements:
//   - Ollama installed and running  →  https://ollama.com
//   - At least one model pulled, e.g.:
//       ollama pull codellama       (best for code analysis, ~4 GB)
//       ollama pull llama3          (good general purpose, ~4 GB)
//       ollama pull deepseek-coder  (alternative code model)
//
// Setup:
//   1. Copy this file to your user scripts folder:  C:\Users\<you>\ghidra_scripts\
//      (Create the folder if it does not exist — Ghidra registers it automatically.)
//      Do NOT place it in <Ghidra>/Ghidra/Features/Base/ghidra_scripts/ — that
//      system directory can break OSGi bundle resolution and is wiped on upgrades.
//   2. Start Ollama (it runs in the background automatically after install)
//   3. Place cursor inside a function in Ghidra
//   4. Window > Script Manager > AnalyzeFunction > Run
//
// Change MODEL below to switch between installed Ollama models.
// No API key or internet connection required — runs entirely on your machine.
//
// @category PS2-Recomp
// @author SDBZ Recomp

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public class AnalyzeFunction extends GhidraScript {

    // -------------------------------------------------------------------------
    // Config — change MODEL to any model you have pulled in Ollama
    // -------------------------------------------------------------------------

    private static final String OLLAMA_URL = "http://localhost:11434/api/chat";
    private static final String MODEL      = "codellama";   // or: llama3, deepseek-coder

    // Max MIPS instructions to send (keeps context size reasonable)
    private static final int MAX_INSTRUCTIONS = 120;

    // System prompt — tells the model about the project and expected output format
    private static final String SYSTEM_PROMPT =
        "You are a reverse engineering assistant for a PS2 game decompilation project. " +
        "The game is Super Dragon Ball Z (PS2, 2005), a 3D fighting game. " +
        "You will be given MIPS (EE) assembly and decompiled C code for a function. " +
        "Your job is to:\n" +
        "  1. Describe what the function does in plain English (2-5 sentences).\n" +
        "  2. Suggest a function name using snake_case following the naming convention below.\n" +
        "  3. Identify the module the function belongs to.\n" +
        "  4. Briefly describe parameters and return value if identifiable.\n\n" +
        "NAMING CONVENTION:\n" +
        "  Format:  [module]_[subsystem]_[action]\n" +
        "  Modules: battle_, char_, player_, input_, move_, anim_, cam_, fx_, ui_,\n" +
        "           audio_, physics_, ki_, ai_, story_, net_, mem_, gfx_, file_, init_,\n" +
        "           debug_, unk_\n" +
        "  Character sub-prefix: char_goku_, char_vegeta_, char_gohan_, char_piccolo_,\n" +
        "           char_trunks_, char_cell_, char_frieza_, char_buu_, char_broly_,\n" +
        "           char_krillin_, char_yamcha_, char_tien_\n" +
        "  Common verbs: init, update, draw, load, reset, check, get, set, calc,\n" +
        "           spawn, destroy, play, stop, handle, process, transition\n\n" +
        "RESPONSE FORMAT (always use exactly these labels, one per line):\n" +
        "DESCRIPTION: <plain English description>\n" +
        "SUGGESTED NAME: <snake_case name>\n" +
        "MODULE: <module prefix>\n" +
        "PARAMS/RETURN: <brief notes, or Unknown>\n" +
        "STATUS: named";

    // -------------------------------------------------------------------------
    // Entry point
    // -------------------------------------------------------------------------

    @Override
    public void run() throws Exception {

        // 1. Find function at cursor
        Function func = getFunctionContaining(currentAddress);
        if (func == null) {
            popup("No function found at the current address.\nPlace your cursor inside a function and try again.");
            return;
        }

        println("=== AnalyzeFunction (Ollama) ===");
        println("Function : " + func.getName());
        println("Address  : " + func.getEntryPoint());
        println("Model    : " + MODEL);

        // 2. Decompile
        println("Decompiling...");
        String decompiledC = decompileFunction(func);

        // 3. Get MIPS assembly
        String assembly = getAssembly(func);

        // 4. Build prompt
        String userPrompt = buildPrompt(func, decompiledC, assembly);

        // 5. Call Ollama
        println("Sending to Ollama (" + MODEL + ") — this may take 10-30 seconds...");
        String analysis = callOllama(userPrompt);

        if (analysis == null || analysis.trim().isEmpty()) {
            popup(
                "No response from Ollama.\n\n" +
                "Make sure Ollama is running and the model is installed:\n" +
                "  ollama pull " + MODEL + "\n\n" +
                "Check the Ghidra console for error details."
            );
            return;
        }

        // 6. Write as plate comment
        String comment = "[ AI ANALYSIS — Ollama/" + MODEL + " ]\n" + analysis.trim();
        setPlateComment(func.getEntryPoint(), comment);

        println("--- Analysis ---");
        println(analysis);
        println("Plate comment written to " + func.getEntryPoint());
    }

    // -------------------------------------------------------------------------
    // Decompiler
    // -------------------------------------------------------------------------

    private String decompileFunction(Function func) {
        DecompInterface decomp = new DecompInterface();
        try {
            decomp.setOptions(new DecompileOptions());
            decomp.openProgram(currentProgram);
            DecompileResults result = decomp.decompileFunction(func, 60, monitor);
            if (result != null && result.decompileCompleted()) {
                return result.getDecompiledFunction().getC();
            }
            return "[Decompilation failed or timed out]";
        } finally {
            decomp.dispose();
        }
    }

    // -------------------------------------------------------------------------
    // Assembly listing
    // -------------------------------------------------------------------------

    private String getAssembly(Function func) {
        StringBuilder sb = new StringBuilder();
        Listing listing = currentProgram.getListing();
        AddressSetView body = func.getBody();
        InstructionIterator iter = listing.getInstructions(body, true);
        int count = 0;

        while (iter.hasNext() && count < MAX_INSTRUCTIONS) {
            Instruction instr = iter.next();
            sb.append(String.format("%-12s  %s\n", instr.getAddress(), instr));
            count++;
        }

        if (iter.hasNext()) {
            sb.append("... (truncated at ").append(MAX_INSTRUCTIONS).append(" instructions)\n");
        }

        return sb.toString();
    }

    // -------------------------------------------------------------------------
    // Prompt builder
    // -------------------------------------------------------------------------

    private String buildPrompt(Function func, String decompiledC, String assembly) {
        return "Function: " + func.getName() + "\n"
             + "Entry point: " + func.getEntryPoint() + "\n"
             + "Size: " + func.getBody().getNumAddresses() + " bytes\n\n"
             + "=== DECOMPILED C ===\n"
             + decompiledC + "\n\n"
             + "=== MIPS ASSEMBLY ===\n"
             + assembly;
    }

    // -------------------------------------------------------------------------
    // Ollama API call
    // -------------------------------------------------------------------------

    private String callOllama(String userContent) throws Exception {
        URL url = new URL(OLLAMA_URL);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setRequestMethod("POST");
        conn.setRequestProperty("Content-Type", "application/json");
        conn.setDoOutput(true);
        conn.setConnectTimeout(10000);
        conn.setReadTimeout(120000); // local models can be slow on first run

        // Ollama chat request — stream:false so we get one complete response
        String body = "{"
            + "\"model\":" + toJsonString(MODEL) + ","
            + "\"stream\":false,"
            + "\"messages\":["
            +   "{\"role\":\"system\",\"content\":" + toJsonString(SYSTEM_PROMPT) + "},"
            +   "{\"role\":\"user\",\"content\":" + toJsonString(userContent) + "}"
            + "]"
            + "}";

        try (OutputStream os = conn.getOutputStream()) {
            os.write(body.getBytes(StandardCharsets.UTF_8));
        }

        int status = conn.getResponseCode();
        InputStream is = (status >= 200 && status < 300)
            ? conn.getInputStream()
            : conn.getErrorStream();

        String responseJson = readStream(is);

        if (status < 200 || status >= 300) {
            println("Ollama error (HTTP " + status + "): " + responseJson);
            return null;
        }

        return extractContent(responseJson);
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private String readStream(InputStream is) throws Exception {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(is, StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                sb.append(line).append("\n");
            }
        }
        return sb.toString();
    }

    /**
     * Extract the assistant's reply from Ollama's response JSON.
     * Ollama returns: {"message":{"role":"assistant","content":"..."},...}
     */
    private String extractContent(String json) {
        // Look for "content":"..." inside the message object
        String marker = "\"content\":\"";
        int start = json.indexOf(marker);
        if (start < 0) {
            println("Could not locate 'content' field in Ollama response:\n" + json);
            return null;
        }

        start += marker.length();
        StringBuilder result = new StringBuilder();
        boolean escaped = false;

        for (int i = start; i < json.length(); i++) {
            char c = json.charAt(i);
            if (escaped) {
                switch (c) {
                    case 'n':  result.append('\n'); break;
                    case 't':  result.append('\t'); break;
                    case 'r':  break;
                    case '"':  result.append('"');  break;
                    case '\\': result.append('\\'); break;
                    default:   result.append('\\').append(c); break;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            } else {
                result.append(c);
            }
        }

        return result.toString();
    }

    /** Escape a Java string for safe embedding in a JSON string literal. */
    private String toJsonString(String s) {
        if (s == null) return "\"\"";
        StringBuilder sb = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        sb.append("\"");
        return sb.toString();
    }
}
