package com.droidspaces.app.util

import android.util.Log
import androidx.compose.runtime.mutableStateListOf
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** One bridge a gateway container is offering, with how many ports are on it. */
data class LanBridge(val name: String, val members: Int)

/**
 * The host's own membership in a container LAN, as "droidspaces net status"
 * reports it. The backend owns the veth, the DHCP lease and the routing rule,
 * and re-asserts them from the container monitors when a gateway restarts.
 *
 * [bridges] is filled whether or not the host is joined: an off switch with a
 * LAN waiting for it and an off switch with no LAN at all are two different
 * things to show.
 */
data class HostLanStatus(
    val joined: Boolean,
    val bridge: String,
    val bridgeUp: Boolean,
    val iface: String,
    val ifaceUp: Boolean,
    val peerUp: Boolean,
    val ip: String,
    val prefix: Int,
    val gateway: String,
    val dhcp: Boolean,
    val bridges: List<LanBridge>,
) {
    /** The LAN this host sits on, in CIDR form, or null when it has no lease. */
    val subnet: String? get() = if (ip.isBlank() || prefix <= 0) null else "$ip/$prefix"

    /** An address, as opposed to a lease that has not come back yet. */
    val hasAddress: Boolean get() = joined && ip.isNotBlank()
}

/**
 * One host-port → container-port rule.
 *
 * Deliberately not `PortForward` from ContainerManager: that one belongs to a
 * container's config and keeps its ports as strings (an empty container port
 * means "same as the host port"). These are the numbers iptables is given, and
 * the target address is resolved from the container's live LAN address.
 */
data class HostForward(
    val proto: String,
    val hostPort: Int,
    val container: String,
    val containerIp: String,
    val containerPort: Int,
    /** How many ports the rule covers, one by default. 2000-2010 is 11. */
    val count: Int = 1,
    /** A human's label for the service behind the rule. Never reaches iptables. */
    val note: String = "",
) {
    val hostLast: Int get() = hostPort + count - 1
    val containerLast: Int get() = containerPort + count - 1

    val hostLabel: String
        get() = if (count <= 1) "$hostPort" else "$hostPort-$hostLast"

    val containerLabel: String
        get() = if (count <= 1) "$containerPort" else "$containerPort-$containerLast"

    /** Same ports on both sides need no second port number in the list. */
    val targetLabel: String
        get() = if (containerLabel == hostLabel) container else "$container:$containerLabel"
}

/**
 * A running container, the address to prefer when pointing something at it, and
 * every address it holds. A gateway container has two, and a rule aimed at
 * either one is still aimed at the container.
 */
data class RunningContainer(
    val name: String,
    val ip: String,
    val addresses: List<String> = emptyList(),
    val pid: Int = 0,
) {
    fun holds(address: String): Boolean =
        addresses.isEmpty() || address in addresses

    /**
     * Whether [address] is on the same segment as the preferred one. A gateway
     * container answers on both its LAN address and its NAT one, and those are
     * not a "move": rewriting a rule between them would tie it to a bridge the
     * host may leave.
     */
    fun sameSubnet(address: String): Boolean =
        ip.isEmpty() || address.isEmpty() ||
            address.substringBeforeLast('.') == ip.substringBeforeLast('.')
}

/** Whether a saved rule is actually doing anything right now. */
enum class ForwardState {
    /** DNAT and the FORWARD accept are both in the kernel, aimed at the right address. */
    ACTIVE,

    /** Saved, but the chains do not carry it: forwarding is off, or it was lost. */
    INACTIVE,

    /** The container is up on a different address than the rule points at. */
    DRIFTED,

    /** The rule is in the kernel, but the container it points at is not running. */
    STOPPED,
}

data class ForwardStatus(val rule: HostForward, val state: ForwardState)

/**
 * Host ↔ container plumbing that belongs to the host, not to a container:
 * joining the containers' LAN, and forwarding host ports into it.
 *
 * The join is a backend command (one binary owns the veth and the lease). The
 * forwards are plain iptables rules, kept in a file of their own so the app can
 * re-assert them after a reboot without a daemon: the kernel forgets them, the
 * file does not.
 *
 * Everything under /data/local/Droidspaces belongs to root, so every read and
 * write here goes through the root shell. Opening those files as the app's own
 * UID fails with EACCES.
 */
object HostNetworkManager {
    private const val CHAIN_NAT = "DSPF_NAT"
    private const val CHAIN_FWD = "DSPF_FWD"
    private const val TAG = "dspaces:net"
    private const val FORWARDS_PATH = "${Constants.WORKSPACE_PATH}/Net/forwards.conf"
    private const val SWITCH_PATH = "${Constants.WORKSPACE_PATH}/Net/forwarding.conf"
    private const val ROUTE_PATH = "${Constants.WORKSPACE_PATH}/Net/route.conf"

    /** What the last operations did, for the log viewer. */
    val logs = mutableStateListOf<Pair<Int, String>>()

    fun clearLogs() = logs.clear()

    private fun info(message: String) {
        Log.i("HostNetwork", message)
        logs.add(Log.INFO to message)
    }

    private fun warn(message: String) {
        Log.w("HostNetwork", message)
        logs.add(Log.WARN to message)
    }

    private fun fail(message: String) {
        Log.e("HostNetwork", message)
        logs.add(Log.ERROR to message)
    }

    private suspend fun sh(command: String): Shell.Result =
        withContext(Dispatchers.IO) { Shell.cmd(command).exec() }

    /** Runs [command] and returns its stdout lines, empty when it failed. */
    private suspend fun shOut(command: String): List<String> {
        val result = sh(command)
        return if (result.isSuccess) result.out else emptyList()
    }

    private var probed = false

    private suspend fun waitFlag(): String {
        if (!probed) {
            probed = true
            /* Android's iptables is 1.8.x in most builds and honours -w; the
             * odd vendor build errors on the flag, so ask once. */
            if (sh("iptables -w -L -n >/dev/null 2>&1").isSuccess)
                Constants.IPTABLES_WAIT = "-w"
        }
        return Constants.IPTABLES_WAIT
    }

    private suspend fun iptables(args: String): Boolean =
        sh("iptables ${waitFlag()} $args >/dev/null 2>&1").isSuccess

    /** Writes [body] to [path] as root; the file and its directory are root-owned. */
    private suspend fun writeRootFile(path: String, body: String): Boolean {
        val dir = ContainerCommandBuilder.quote(path.substringBeforeLast('/'))
        return sh(
            "mkdir -p $dir && printf '%s\\n' ${ContainerCommandBuilder.quote(body)} " +
                "> ${ContainerCommandBuilder.quote(path)} 2>/dev/null"
        ).isSuccess
    }

    private suspend fun readRootFile(path: String): List<String> =
        shOut("cat ${ContainerCommandBuilder.quote(path)} 2>/dev/null")

    // ── Host LAN ────────────────────────────────────────────────────────────

    suspend fun status(): HostLanStatus? {
        val result = sh("${Constants.DROIDSPACES_BINARY_PATH} --json net status 2>/dev/null")
        val line = result.out.firstOrNull { it.trimStart().startsWith("{") } ?: return null
        return runCatching {
            val o = JSONObject(line)
            val bridges = o.optJSONArray("bridges")?.let { array ->
                (0 until array.length()).mapNotNull { i ->
                    val b = array.optJSONObject(i) ?: return@mapNotNull null
                    LanBridge(b.optString("name"), b.optInt("members"))
                }
            }.orEmpty()
            HostLanStatus(
                joined = o.optInt("joined") == 1,
                bridge = o.optString("bridge"),
                bridgeUp = o.optInt("bridge_up") == 1,
                iface = o.optString("iface"),
                ifaceUp = o.optInt("iface_up") == 1,
                peerUp = o.optInt("peer_up") == 1,
                ip = o.optString("ip"),
                prefix = o.optInt("prefix"),
                gateway = o.optString("gateway"),
                dhcp = o.optInt("dhcp") == 1,
                bridges = bridges,
            )
        }.getOrNull()
    }

    /** Plugs the host into [bridge], or into the backend's default when null. */
    suspend fun join(bridge: String? = null): Boolean {
        val target = bridge?.takeIf { it.isNotBlank() }
        info(if (target == null) "Joining the container LAN…" else "Joining $target…")
        val command = buildString {
            append(Constants.DROIDSPACES_BINARY_PATH).append(" net join")
            // A positional: the backend's option parser rejects --bridge.
            if (target != null) append(' ').append(ContainerCommandBuilder.quote(target))
        }
        val ok = sh("$command 2>&1").isSuccess
        if (ok) info("Joined.") else fail("Could not join the container LAN.")
        return ok
    }

    suspend fun leave(): Boolean {
        info("Leaving the container LAN…")
        val ok = sh("${Constants.DROIDSPACES_BINARY_PATH} net leave 2>&1").isSuccess
        if (ok) info("Left.") else fail("Could not leave the container LAN.")
        return ok
    }

    /**
     * Gives the gateway a way back to the network the forwarded connections come
     * from. Without it a container answers through the phone's NAT bridge, whose
     * source rewrite no client recognises, and the forwarding looks one-way.
     *
     * Runs against the gateway container — the one holding the LAN's gateway
     * address — and replaces whatever route for that subnet is there, including
     * the 192.168.100.50 one the old tools left behind.
     */
    suspend fun syncGatewayRoute(
        status: HostLanStatus?,
        containers: List<RunningContainer>,
    ): Boolean = runCatching {
        val st = status ?: return@runCatching false
        if (!st.hasAddress || st.gateway.isBlank() || st.gateway == "0.0.0.0")
            return@runCatching false

        // Match on every address the container holds: the preferred one is the
        // LAN address only when the caller knew the LAN prefix, and the gateway
        // is also the container that owns the bridge's NAT side.
        val gateway = containers.firstOrNull { it.addresses.contains(st.gateway) }
            ?: containers.firstOrNull { it.ip == st.gateway }
        if (gateway == null) {
            warn("No container holds ${st.gateway}; the return route is not in place.")
            return@runCatching false
        }

        // The subnet we forward from is the one the uplink sits on.
        val dev = Regex("dev (\\S+)")
            .find(shOut("ip route get 8.8.8.8 2>/dev/null").joinToString(" "))
            ?.groupValues?.get(1)
        if (dev == null) {
            warn("No uplink found; the gateway cannot return to the client network.")
            return@runCatching false
        }
        if (dev.startsWith("rmnet") || dev.startsWith("ccmni") || dev.startsWith("pdp")) {
            info("Uplink is $dev (mobile data); no return route is useful there.")
            return@runCatching true
        }

        val cidr = shOut("ip -4 -o addr show dev $dev 2>/dev/null")
            .firstOrNull()
            ?.split(Regex("\\s+"))
            ?.let { tokens -> tokens.getOrNull(tokens.indexOf("inet") + 1) }
            ?: return@runCatching false
        val subnet = networkCidr(cidr) ?: return@runCatching false

        /* What we last told this gateway. Running a command inside a container
         * costs the better part of a second, and the answer only changes when
         * the gateway restarts (new pid), the phone gets a new LAN address, or
         * the uplink moves to another subnet. */
        val record = "gw=${gateway.name} pid=${gateway.pid} via=${st.ip} subnet=$subnet"
        if (readRootFile(ROUTE_PATH).any { it.trim() == record }) {
            info("${gateway.name}: $subnet via ${st.ip} already in place.")
            return@runCatching false
        }

        val command = "${Constants.DROIDSPACES_BINARY_PATH} -n " +
            "${ContainerCommandBuilder.quote(gateway.name)} run -- " +
            "ip route replace $subnet via ${st.ip}"
        if (!sh("$command 2>&1").isSuccess) {
            fail("Could not point ${gateway.name} at $subnet via ${st.ip}.")
            return@runCatching false
        }
        writeRootFile(ROUTE_PATH, record)
        info("${gateway.name}: $subnet via ${st.ip}")
        true
    }.getOrDefault(false)

    /** "192.168.124.111/24" → "192.168.124.0/24", or null when it is not IPv4. */
    private fun networkCidr(cidr: String): String? {
        val parts = cidr.split('/')
        val octets = parts.getOrNull(0)?.split('.')?.mapNotNull { it.toIntOrNull() }
        val prefix = parts.getOrNull(1)?.toIntOrNull()
        if (octets == null || octets.size != 4 || prefix == null || prefix !in 1..32)
            return null
        val address = octets.fold(0L) { acc, octet -> (acc shl 8) or octet.toLong() }
        val mask = (0xFFFFFFFFL shl (32 - prefix)) and 0xFFFFFFFFL
        val network = address and mask
        return "${(network shr 24) and 0xFF}.${(network shr 16) and 0xFF}." +
            "${(network shr 8) and 0xFF}.${network and 0xFF}/$prefix"
    }

    // ── Running containers and their addresses ──────────────────────────────

    /**
     * Running containers with the address they answer on. A gateway client
     * reports its LAN address first; a NAT container reports its 172.28.x.y.
     */
    suspend fun runningContainers(lanPrefix: String?): List<RunningContainer> {
        val result = sh("${Constants.DROIDSPACES_BINARY_PATH} --json show 2>/dev/null")
        val line = result.out.firstOrNull { it.trimStart().startsWith("{") } ?: return emptyList()
        val root = runCatching { JSONObject(line) }.getOrNull() ?: return emptyList()
        val running = root.optJSONArray("running") ?: return emptyList()
        return (0 until running.length()).mapNotNull { i ->
            val o = running.optJSONObject(i) ?: return@mapNotNull null
            val name = o.optString("name")
            val addrs = o.optString("ip").split(',').map { it.trim() }.filter { it.isNotEmpty() }
            val ip = preferredAddress(addrs, lanPrefix)
            if (name.isEmpty()) null
            else RunningContainer(name, ip, addrs, o.optInt("pid"))
        }
    }

    /**
     * Which of a container's addresses to aim at. Prefer the LAN the host is
     * actually joined to, then trust the order the backend reports: it lists
     * its own cables (eth0, eth1) before any bridge the guest created itself.
     */
    private fun preferredAddress(addresses: List<String>, lanPrefix: String?): String {
        if (lanPrefix != null)
            addresses.firstOrNull { it.startsWith(lanPrefix) }?.let { return it }
        /* The backend lists the runtime's own cables (eth0, then eth1) ahead of
         * anything the guest made for itself, so the first address is always
         * one a forward can reach. Guessing by range here used to skip this
         * runtime's own NAT network (172.28.0.0/16 sits in docker's pool). */
        return addresses.firstOrNull().orEmpty()
    }

    // ── Forwards: the file is the truth, iptables is derived from it ────────

    /** Whether forwarding is switched on at all. On unless someone turned it off. */
    suspend fun forwardingEnabled(): Boolean {
        val line = readRootFile(SWITCH_PATH).firstOrNull { it.contains('=') } ?: return true
        // Accept what we write (1/0) and what a human would type (on/off).
        val value = line.substringAfter('=').trim().lowercase()
        return value !in setOf("0", "off", "false", "no")
    }

    /** Off keeps the rules on disk and empties the chains; on puts them back. */
    suspend fun setForwardingEnabled(enabled: Boolean): Boolean = runCatching {
        if (!writeRootFile(SWITCH_PATH, "enabled=${if (enabled) 1 else 0}")) {
            fail("Could not save the forwarding switch.")
            return@runCatching false
        }
        info(if (enabled) "Port forwarding on." else "Port forwarding off.")
        if (enabled) applyAll() else {
            ensureChains()
            flushChains()
            info("All rules removed from the kernel; the list is kept.")
        }
        true
    }.getOrDefault(false)

    suspend fun listForwards(): List<HostForward> =
        readRootFile(FORWARDS_PATH).mapNotNull { line ->
            val trimmed = line.trim()
            if (trimmed.isEmpty() || trimmed.startsWith("#")) return@mapNotNull null
            val parts = trimmed.split('|')
            // Five fields is an old rule written before notes existed.
            if (parts.size !in 5..6) return@mapNotNull null
            val host = portSpan(parts[1]) ?: return@mapNotNull null
            val container = portSpan(parts[4]) ?: return@mapNotNull null
            /* Both sides move together: one host port per container port. A
             * single container port is repeated across the span. */
            HostForward(
                proto = parts[0],
                hostPort = host.first,
                container = parts[2],
                containerIp = parts[3],
                containerPort = container.first,
                count = host.second,
                note = parts.getOrNull(5).orEmpty(),
            )
        }

    /** "8080" or "2000-2010" to (first, count), null when it is neither. */
    private fun portSpan(text: String): Pair<Int, Int>? {
        val trimmed = text.trim()
        if ('-' !in trimmed) {
            val port = trimmed.toIntOrNull() ?: return null
            return if (port in 1..65535) port to 1 else null
        }
        val bounds = trimmed.split('-', limit = 2)
        val from = bounds[0].toIntOrNull() ?: return null
        val to = bounds.getOrNull(1)?.toIntOrNull() ?: return null
        if (from !in 1..65535 || to !in from..65535) return null
        return from to (to - from + 1)
    }

    /** Keep a note to one line and out of the field separator. */
    private fun noteField(text: String): String =
        text.trim().replace('|', ' ').replace('\n', ' ').replace('\r', ' ')

    private suspend fun writeForwards(rules: List<HostForward>): Boolean {
        val lines = buildList {
            add("# Host port forwards, one per line: proto|host|container|ip|port[|note]")
            rules.forEach {
                val note = noteField(it.note)
                val suffix = if (note.isEmpty()) "" else "|$note"
                add("${it.proto}|${it.hostLabel}|${it.container}|${it.containerIp}|${it.containerLabel}$suffix")
            }
        }
        return writeRootFile(FORWARDS_PATH, lines.joinToString("\n"))
    }

    /**
     * Whether each saved rule is really in the kernel. One dump for the whole
     * list: a rule that survived a reboot of the phone or a restart of the
     * container looks identical in the file, and only the chains know.
     */
    /** Our rules as they exist right now, in one shell call. */
    private suspend fun chainLines(): Pair<List<String>, List<String>> {
        val wait = waitFlag()
        val dump = shOut(
            "iptables $wait -t nat -S $CHAIN_NAT 2>/dev/null; echo @@; " +
                "iptables $wait -S $CHAIN_FWD 2>/dev/null"
        )
        val split = dump.indexOf("@@")
        val nat = (if (split >= 0) dump.subList(0, split) else dump).filter { it.contains(TAG) }
        val forward = (if (split >= 0) dump.drop(split + 1) else emptyList())
            .filter { it.contains(TAG) }
        return nat to forward
    }

    /** Every port in the rule, one line per port, is what the kernel holds. */
    private fun hasRedirect(nat: List<String>, rule: HostForward): Boolean =
        (0 until rule.count).all { offset ->
            val host = rule.hostPort + offset
            val inside = rule.containerPort + offset
            nat.any { line ->
                line.contains("-p ${rule.proto}") &&
                    line.contains("--dport $host ") &&
                    line.contains("--to-destination ${rule.containerIp}:$inside")
            }
        }

    private fun hasAccept(forward: List<String>, rule: HostForward): Boolean =
        (0 until rule.count).all { offset ->
            val inside = rule.containerPort + offset
            forward.any { line ->
                line.contains("-d ${rule.containerIp}") &&
                    line.contains("--dport $inside ") &&
                    line.contains("-j ACCEPT")
            }
        }

    suspend fun forwardStates(
        rules: List<HostForward>,
        containers: List<RunningContainer>,
        forwardingOn: Boolean,
    ): List<ForwardStatus> {
        if (rules.isEmpty()) return emptyList()
        val (nat, forward) = chainLines()
        return rules.map { rule ->
            val live = containers.firstOrNull { it.name == rule.container }
            /* The switch decides first: with forwarding off there is nothing in
             * the kernel, so "not applied" is the truth about every rule,
             * whatever the container is doing. */
            val state = when {
                !forwardingOn -> ForwardState.INACTIVE
                live == null -> ForwardState.STOPPED
                !live.holds(rule.containerIp) && live.sameSubnet(rule.containerIp) ->
                    ForwardState.DRIFTED
                hasRedirect(nat, rule) && hasAccept(forward, rule) -> ForwardState.ACTIVE
                else -> ForwardState.INACTIVE
            }
            ForwardStatus(rule, state)
        }
    }

    /** Adds the rule to the file and to the kernel. False when either refused. */
    suspend fun addForward(rule: HostForward, subnet: String?): Boolean = runCatching {
        info("Forwarding ${rule.hostPort} → ${rule.container}:${rule.containerPort}/${rule.proto}")
        if (!install(rule, subnet)) {
            fail("iptables refused the rule for ${rule.hostPort}.")
            return@runCatching false
        }
        val rest = listForwards().filterNot { it.hostPort == rule.hostPort && it.proto == rule.proto }
        if (!writeForwards(rest + rule)) {
            fail("The rule is in the kernel but could not be saved.")
            return@runCatching false
        }
        info("Rule saved.")
        true
    }.getOrDefault(false)

    /**
     * Replace a rule in the kernel and in the file. The old one is removed
     * first so an edit that only changes the target still fits: two DNATs for
     * the same host port would fight over the packet.
     */
    suspend fun updateForward(
        old: HostForward,
        updated: HostForward,
        subnet: String?,
    ): Boolean = runCatching {
        info("Editing ${old.hostLabel} → ${old.container}:${old.containerLabel}")
        uninstall(old)
        if (!install(updated, subnet)) {
            uninstall(updated)
            install(old, subnet)
            fail("iptables refused the edited rule.")
            return@runCatching false
        }
        val rest = listForwards()
            .filterNot {
                it.hostPort == old.hostPort && it.proto == old.proto &&
                    it.containerIp == old.containerIp
            }
            .filterNot { it.hostPort == updated.hostPort && it.proto == updated.proto }
        if (!writeForwards(rest + updated)) {
            uninstall(updated)
            install(old, subnet)
            fail("The edited rule could not be saved.")
            return@runCatching false
        }
        info("Rule saved.")
        true
    }.getOrDefault(false)

    suspend fun removeForward(rule: HostForward): Boolean = runCatching {
        info("Removing ${rule.hostPort} → ${rule.container}:${rule.containerPort}")
        uninstall(rule)
        writeForwards(listForwards().filterNot {
            it.hostPort == rule.hostPort && it.proto == rule.proto && it.containerIp == rule.containerIp
        })
    }.getOrDefault(false)

    /**
     * Re-asserts every saved rule. Called when the tab opens, after a container
     * restarts and when forwarding is switched back on.
     *
     * The file is the truth, so the chains are emptied first: a rule whose write
     * never landed, or a forward into a container that is gone, would otherwise
     * stay in the kernel for good. Rules whose container came back on a new
     * address are rewritten before they are installed.
     */
    suspend fun applyAll(): Boolean = runCatching {
        if (!forwardingEnabled()) {
            val (nat, forward) = chainLines()
            if (nat.isEmpty() && forward.isEmpty()) {
                info("Forwarding is off; no rules in the kernel.")
                return@runCatching false
            }
            ensureChains()
            flushChains()
            info("Forwarding is off; rules removed from the kernel.")
            return@runCatching true
        }

        val containers = runningContainers(null)
        val saved = listForwards()
        val followed = saved.map { rule ->
            val live = containers.firstOrNull { it.name == rule.container }
            if (live != null && live.ip.isNotEmpty() && !live.holds(rule.containerIp) &&
                live.sameSubnet(rule.containerIp)
            ) {
                info("${rule.container} moved to ${live.ip}; rule ${rule.hostPort} follows it.")
                rule.copy(containerIp = live.ip)
            } else {
                rule
            }
        }
        if (followed != saved) writeForwards(followed)

        /* Read before writing: on a phone that has not rebooted the kernel
         * already matches the file, and re-installing is a dozen root shells
         * the user would wait through every time the tab opens. */
        val (nat, forward) = chainLines()
        // A span writes one chain line per port, so the counts to match are
        // the ports, not the rules.
        val ports = followed.sumOf { it.count }
        val upToDate = followed.all { hasRedirect(nat, it) && hasAccept(forward, it) } &&
            nat.size == ports && forward.size == ports
        if (upToDate) {
            if (followed.isNotEmpty())
                info("${followed.size} rule${if (followed.size == 1) "" else "s"} already applied.")
            return@runCatching false
        }

        ensureChains()
        flushChains()
        if (followed.isEmpty()) return@runCatching true
        val subnet = status()?.subnet
        followed.forEach { install(it, subnet) }
        info("${followed.size} rule${if (followed.size == 1) "" else "s"} applied.")
        true
    }.getOrDefault(false)

    private suspend fun ensureChains() {
        iptables("-t nat -N $CHAIN_NAT")
        iptables("-N $CHAIN_FWD")
        if (!iptables("-t nat -C PREROUTING -j $CHAIN_NAT"))
            iptables("-t nat -I PREROUTING 1 -j $CHAIN_NAT")
        if (!iptables("-C FORWARD -j $CHAIN_FWD"))
            iptables("-I FORWARD 1 -j $CHAIN_FWD")
    }

    private suspend fun flushChains() {
        iptables("-t nat -F $CHAIN_NAT")
        iptables("-F $CHAIN_FWD")
    }

    private suspend fun install(rule: HostForward, subnet: String?): Boolean {
        ensureChains()

        for (offset in 0 until rule.count) {
            val host = rule.hostPort + offset
            val inside = rule.containerPort + offset

            val dnat = "$CHAIN_NAT -p ${rule.proto} --dport $host " +
                "-m comment --comment $TAG -j DNAT --to-destination ${rule.containerIp}:$inside"
            if (!iptables("-t nat -C $dnat") && !iptables("-t nat -A $dnat"))
                return false

            val accept = "$CHAIN_FWD -p ${rule.proto} -d ${rule.containerIp} " +
                "--dport $inside -m comment --comment $TAG -j ACCEPT"
            if (!iptables("-C $accept")) iptables("-A $accept")
        }

        // A container's reply has to come back out the same way; the gateway
        // container's own NAT only covers traffic it routes itself.
        if (subnet != null) {
            val masq = "POSTROUTING -s $subnet ! -d $subnet " +
                "-m comment --comment $TAG:masq -j MASQUERADE"
            if (!iptables("-t nat -C $masq")) iptables("-t nat -A $masq")
        }

        // Android's tethering rules drop forwarded packets; without this the
        // DNAT above matches and the packet still dies in FORWARD.
        var guard = 0
        while (guard++ < 8 && iptables("-D tetherctrl_FORWARD -j DROP")) {
            /* remove every copy */
        }
        return true
    }

    private suspend fun uninstall(rule: HostForward) {
        for (offset in 0 until rule.count) {
            val host = rule.hostPort + offset
            val inside = rule.containerPort + offset
            iptables(
                "-t nat -D $CHAIN_NAT -p ${rule.proto} --dport $host " +
                    "-m comment --comment $TAG -j DNAT --to-destination ${rule.containerIp}:$inside"
            )
            iptables(
                "-D $CHAIN_FWD -p ${rule.proto} -d ${rule.containerIp} --dport $inside " +
                    "-m comment --comment $TAG -j ACCEPT"
            )
        }
    }
}
