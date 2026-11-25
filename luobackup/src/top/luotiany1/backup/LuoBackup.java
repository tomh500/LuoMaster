package top.luotiany1.backup;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Proxy;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.UUID;
import net.md_5.bungee.api.CommandSender;
import net.md_5.bungee.api.chat.TextComponent;
import net.md_5.bungee.api.plugin.Command;
import net.md_5.bungee.api.plugin.Plugin;
import net.md_5.bungee.config.Configuration;
import net.md_5.bungee.config.ConfigurationProvider;
import net.md_5.bungee.config.YamlConfiguration;

public final class LuoBackup extends Plugin {
    @Override
    public void onEnable() {
        try {
            Path configPath = getDataFolder().toPath().resolve("config.yml");
            Files.createDirectories(configPath.getParent());
            if (!Files.exists(configPath)) {
                try (InputStream defaults = getResourceAsStream("config.yml")) {
                    Files.copy(defaults, configPath);
                }
            }
            Configuration config = ConfigurationProvider.getProvider(YamlConfiguration.class).load(configPath.toFile());
            if (!config.getBoolean("master.enabled", false)) {
                getLogger().warning("LuoBackup requires an explicit Master configuration. No backup requests are scheduled.");
                return;
            }
            int port = config.getInt("master.port", 18776);
            Path tokenFile = Paths.get(config.getString("master.token-file", ""));
            if (!tokenFile.isAbsolute() || port < 1024 || port > 65535 || Files.size(tokenFile) > 130) {
                throw new IOException("Invalid Master port or token-file");
            }
            String token = new String(Files.readAllBytes(tokenFile), StandardCharsets.US_ASCII).replaceFirst("\\r?\\n$", "");
            if (!token.matches("[!-~]{32,128}")) throw new IOException("Invalid Master token format");
            Configuration targets = config.getSection("targets");
            getProxy().getPluginManager().registerCommand(this, new Command("luobackup", "luobackup.manual") {
                @Override
                public void execute(CommandSender sender, String[] args) {
                    if (args.length == 2 && "status".equalsIgnoreCase(args[0]) && validId(args[1])) {
                        dispatch(sender, port, token, args[1], null);
                        return;
                    }
                    if ((args.length != 2 && args.length != 3) || !"now".equalsIgnoreCase(args[0]) ||
                        !validId(args[1]) || (args.length == 3 && !validId(args[2]))) {
                        sender.sendMessage(new TextComponent("/luobackup now <server> [operation-id] or /luobackup status <operation-id>"));
                        return;
                    }
                    String profile = targets.getString(args[1], "");
                    if (!validId(profile)) {
                        sender.sendMessage(new TextComponent("No Master backup profile configured for " + args[1]));
                        return;
                    }
                    String id = args.length == 3 ? args[2] : UUID.randomUUID().toString();
                    getLogger().info("Backup authorization requested by " + sender.getName() + " for " + profile + " (" + id + ")");
                    sender.sendMessage(new TextComponent("Requesting maintenance approval from Master. Operation ID: " + id));
                    dispatch(sender, port, token, id, profile);
                }
            });
            getLogger().info("LuoBackup Master request client enabled.");
        } catch (IOException | RuntimeException e) {
            getLogger().severe("Cannot enable LuoBackup: " + e.getMessage());
        }
    }

    private void dispatch(CommandSender sender, int port, String token, String id, String profile) {
        getProxy().getScheduler().runAsync(this, () -> {
            try {
                String response = request(port, token, id, profile);
                getLogger().info("Master operation " + id + ": " + response);
                sender.sendMessage(new TextComponent(response));
            } catch (IOException e) {
                String message = "Master request outcome unknown. Query operation " + id + " or retry with the same ID.";
                getLogger().warning(message);
                sender.sendMessage(new TextComponent(message));
            }
        });
    }

    private static boolean validId(String value) {
        return value.matches("[A-Za-z0-9_-]{1,64}");
    }

    private static String request(int port, String token, String id, String profile) throws IOException {
        String path = profile == null ? "/v1/backups/" + id : "/v1/backups";
        URL url = new URL("http://127.0.0.1:" + port + path);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection(Proxy.NO_PROXY);
        try {
            conn.setConnectTimeout(1500);
            conn.setReadTimeout(2500);
            conn.setInstanceFollowRedirects(false);
            conn.setRequestProperty("Authorization", "Bearer " + token);
            if (profile != null) {
                byte[] body = ("{\"profile\":\"" + profile + "\"}").getBytes(StandardCharsets.UTF_8);
                conn.setRequestMethod("POST");
                conn.setRequestProperty("Content-Type", "application/json");
                conn.setRequestProperty("Idempotency-Key", id);
                conn.setFixedLengthStreamingMode(body.length);
                conn.setDoOutput(true);
                try (OutputStream output = conn.getOutputStream()) { output.write(body); }
            }
            int status = conn.getResponseCode();
            try (InputStream input = status >= 400 ? conn.getErrorStream() : conn.getInputStream()) {
                if (input == null) return "Master HTTP " + status;
                ByteArrayOutputStream data = new ByteArrayOutputStream();
                byte[] chunk = new byte[512];
                int read;
                while ((read = input.read(chunk)) != -1) {
                    if (data.size() + read > 4096) throw new IOException("Master response exceeds 4 KiB");
                    data.write(chunk, 0, read);
                }
                return "Master HTTP " + status + " " + data.toString("UTF-8");
            }
        } finally {
            conn.disconnect();
        }
    }
}
