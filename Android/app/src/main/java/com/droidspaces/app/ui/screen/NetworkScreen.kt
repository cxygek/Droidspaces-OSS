package com.droidspaces.app.ui.screen

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Lan
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.annotation.StringRes
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.KeyboardOptions
import com.droidspaces.app.R
import com.droidspaces.app.ui.component.DialogFooterRow
import com.droidspaces.app.ui.component.DsDialog
import com.droidspaces.app.ui.component.DsDropdown
import com.droidspaces.app.ui.component.DsTextFieldDefaults
import com.droidspaces.app.util.ForwardState
import com.droidspaces.app.util.ForwardStatus
import com.droidspaces.app.util.HostForward
import com.droidspaces.app.util.HostLanStatus
import com.droidspaces.app.util.HostNetworkManager
import com.droidspaces.app.util.LanBridge
import com.droidspaces.app.util.RunningContainer
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** How long to keep asking for a lease before telling the user it is late. */
private const val LEASE_POLLS = 40
private const val LEASE_POLL_MS = 300L

/**
 * The phone's own side of the container network: the LAN it is plugged into,
 * and the host ports that reach into it.
 *
 * Both halves are host-level, not per container, which is why they live in a
 * tab of their own: a forward belongs to the phone's port 8080, and the LAN
 * membership belongs to the phone's interface, no matter how many containers
 * come and go behind them.
 */
@Composable
fun NetworkScreen(modifier: Modifier = Modifier, bottomBarInset: Dp = 0.dp) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var status by remember { mutableStateOf<HostLanStatus?>(null) }
    var forwards by remember { mutableStateOf<List<ForwardStatus>>(emptyList()) }
    var running by remember { mutableStateOf<List<RunningContainer>>(emptyList()) }
    var forwardingOn by remember { mutableStateOf(true) }
    var busyLan by remember { mutableStateOf(false) }
    var busyForwarding by remember { mutableStateOf(false) }
    var adding by remember { mutableStateOf(false) }
    var editing by remember { mutableStateOf<HostForward?>(null) }
    var forwardSort by remember { mutableStateOf(ForwardSort.PORT) }
    var chosenBridge by remember { mutableStateOf<String?>(null) }
    var notice by remember { mutableStateOf<String?>(null) }

    suspend fun refresh() {
        val st = HostNetworkManager.status()
        status = st
        if (chosenBridge == null) chosenBridge = st?.bridges?.firstOrNull()?.name
        val lanPrefix = st?.ip?.takeIf { it.isNotBlank() }?.substringBeforeLast('.')?.plus(".")
        running = HostNetworkManager.runningContainers(lanPrefix)
        forwardingOn = HostNetworkManager.forwardingEnabled()
        forwards = HostNetworkManager.forwardStates(
            HostNetworkManager.listForwards(), running, forwardingOn)
    }

    LaunchedEffect(Unit) {
        // Read first, repair second. The card can show the lease, the rules and
        // their real state from two cheap reads; re-installing rules or running
        // a command inside the gateway is only worth doing once one of those
        // reads says something is off, and both steps check before they touch
        // anything.
        refresh()
        if (HostNetworkManager.applyAll()) refresh()
        // A container that came back after a reboot also came back without the
        // route that makes forwarded connections answerable.
        HostNetworkManager.syncGatewayRoute(status, running)
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 12.dp + bottomBarInset),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        HostLanCard(
            status = status,
            busy = busyLan,
            chosen = chosenBridge,
            onChoose = { chosenBridge = it },
            onToggle = { want ->
                scope.launch {
                    busyLan = true
                    notice = null
                    if (want) {
                        if (!HostNetworkManager.join(chosenBridge)) {
                            notice = context.getString(R.string.host_lan_failed)
                        } else {
                            // join comes back with a lease, but a gateway that is
                            // still starting can hand one out a heartbeat later.
                            var polls = 0
                            while (polls < LEASE_POLLS && status?.hasAddress != true) {
                                delay(LEASE_POLL_MS)
                                refresh()
                                polls++
                            }
                            if (status?.hasAddress != true)
                                notice = context.getString(R.string.host_lan_late)
                            HostNetworkManager.syncGatewayRoute(status, running)
                        }
                    } else {
                        if (!HostNetworkManager.leave())
                            notice = context.getString(R.string.host_lan_failed)
                    }
                    refresh()
                    busyLan = false
                }
            }
        )

        ForwardingCard(
            enabled = forwardingOn,
            busy = busyForwarding,
            entries = sortedForwards(forwards, forwardSort),
            sort = forwardSort,
            onSort = { forwardSort = it },
            onAdd = { adding = true },
            onDelete = { rule ->
                scope.launch {
                    HostNetworkManager.removeForward(rule)
                    refresh()
                }
            },
            onEdit = { rule -> editing = rule },
            onToggle = { want ->
                scope.launch {
                    busyForwarding = true
                    notice = null
                    if (!HostNetworkManager.setForwardingEnabled(want))
                        notice = context.getString(R.string.port_forward_failed)
                    refresh()
                    busyForwarding = false
                    if (want) HostNetworkManager.syncGatewayRoute(status, running)
                }
            }
        )

        notice?.let {
            Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }

        Spacer(Modifier.height(12.dp))
    }

    editing?.let { rule ->
        /* A stopped container is not in the running list, but its rule still
         * has a name and an address. Keep it selectable so the whole rule can
         * be edited, not only the ports of a live container. */
        val targets = if (running.any { it.name == rule.container }) running
                      else running + RunningContainer(rule.container, rule.containerIp)
        ForwardDialog(
            containers = targets,
            initial = rule,
            onDismiss = { editing = null },
            onConfirm = { updated ->
                editing = null
                scope.launch {
                    if (!HostNetworkManager.updateForward(rule, updated, status?.subnet))
                        notice = context.getString(R.string.port_forward_failed)
                    else
                        HostNetworkManager.syncGatewayRoute(status, running)
                    refresh()
                }
            }
        )
    }

    if (adding) {
        ForwardDialog(
            containers = running,
            onDismiss = { adding = false },
            onConfirm = { rule ->
                adding = false
                scope.launch {
                    if (!HostNetworkManager.addForward(rule, status?.subnet))
                        notice = context.getString(R.string.port_forward_failed)
                    else
                        HostNetworkManager.syncGatewayRoute(status, running)
                    refresh()
                }
            }
        )
    }
}

@Composable
private fun HostLanCard(
    status: HostLanStatus?,
    busy: Boolean,
    chosen: String?,
    onChoose: (String) -> Unit,
    onToggle: (Boolean) -> Unit,
) {
    val context = LocalContext.current
    val joined = status?.joined == true
    val bridges = status?.bridges.orEmpty()

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 0.dp
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Default.Lan,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    context.getString(R.string.host_lan_title),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                Switch(
                    checked = joined,
                    onCheckedChange = onToggle,
                    enabled = !busy && (joined || bridges.isNotEmpty())
                )
            }

            // Which LAN, when there is a choice to make. One bridge needs no
            // picker; several would otherwise be a silent guess.
            if (!joined && bridges.size > 1) {
                val selected = bridges.firstOrNull { it.name == chosen } ?: bridges.first()
                DsDropdown(
                    label = context.getString(R.string.host_lan_bridge_label),
                    selected = selected,
                    options = bridges,
                    displayName = { bridgeLabel(context, it) },
                    onSelect = { onChoose(it.name) },
                    enabled = !busy
                )
            }

            val detail = when {
                status == null -> context.getString(R.string.host_lan_unknown)
                joined && status.hasAddress -> buildString {
                    append(status.ip)
                    if (status.prefix > 0) append("/").append(status.prefix)
                    if (status.gateway.isNotBlank() && status.gateway != "0.0.0.0")
                        append("  ·  ").append(status.gateway)
                    append("  ·  ").append(status.bridge)
                }
                joined -> context.getString(R.string.host_lan_waiting)
                bridges.isEmpty() -> context.getString(R.string.host_lan_no_lan)
                bridges.size == 1 -> buildString {
                    append(bridges.first().name)
                    append("  ·  ")
                    append(context.getString(R.string.host_lan_members, bridges.first().members))
                }
                else -> context.getString(R.string.host_lan_pick)
            }
            Text(
                detail,
                style = MaterialTheme.typography.bodyMedium,
                color = if (joined && status?.hasAddress == true) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurfaceVariant
            )

            if (!joined && bridges.isNotEmpty()) {
                Text(
                    context.getString(R.string.host_lan_intro),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

/**
 * The forwarding master switch and its explanation, in one card: the switch
 * belongs to the port section the way the LAN switch belongs to its own.
 */
@Composable
private fun ForwardingCard(
    enabled: Boolean,
    busy: Boolean,
    entries: List<ForwardStatus>,
    sort: ForwardSort,
    onSort: (ForwardSort) -> Unit,
    onAdd: () -> Unit,
    onDelete: (HostForward) -> Unit,
    onEdit: (HostForward) -> Unit,
    onToggle: (Boolean) -> Unit,
) {
    val context = LocalContext.current
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 0.dp
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Default.SwapHoriz,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    context.getString(R.string.port_forwarding),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
                // How the list below is ordered, then the way to add to it.
                SortChip(sort, enabled = entries.size > 1) { onSort(sort.other()) }
                IconButton(onClick = onAdd, enabled = !busy) {
                    Icon(
                        Icons.Default.Add,
                        contentDescription = context.getString(R.string.port_forward_add),
                        modifier = Modifier.size(20.dp),
                        tint = MaterialTheme.colorScheme.primary
                    )
                }
                Switch(checked = enabled, onCheckedChange = onToggle, enabled = !busy)
            }
            Text(
                context.getString(R.string.port_forwards_intro),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            if (!enabled) {
                Text(
                    context.getString(R.string.forward_off_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )
            }

            when {
                entries.isEmpty() -> Text(
                    context.getString(R.string.port_forwards_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                else -> Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = RULE_LIST_MAX_HEIGHT)
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    entries.forEach { entry ->
                        ForwardRow(
                            entry,
                            sort = sort,
                            onDelete = { onDelete(entry.rule) },
                            onEdit = { onEdit(entry.rule) },
                        )
                    }
                }
            }
        }
    }
}

/** Bounded so a long list scrolls inside the card instead of pushing the page. */
private val RULE_LIST_MAX_HEIGHT = 300.dp

@Composable
private fun SortChip(sort: ForwardSort, enabled: Boolean, onClick: () -> Unit) {
    val context = LocalContext.current
    Surface(
        onClick = onClick,
        enabled = enabled,
        shape = MaterialTheme.shapes.small,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Text(
            context.getString(sort.labelRes),
            style = MaterialTheme.typography.labelMedium,
            color = if (enabled) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp)
        )
    }
}

private enum class ForwardSort(@StringRes val labelRes: Int) {
    PORT(R.string.forward_sort_port),
    CONTAINER(R.string.forward_sort_container);

    fun other(): ForwardSort = if (this == PORT) CONTAINER else PORT
}

private fun sortedForwards(entries: List<ForwardStatus>, sort: ForwardSort): List<ForwardStatus> =
    when (sort) {
        ForwardSort.PORT -> entries.sortedWith(
            compareBy({ it.rule.hostPort }, { it.rule.container.lowercase() })
        )
        ForwardSort.CONTAINER -> entries.sortedWith(
            compareBy({ it.rule.container.lowercase() }, { it.rule.hostPort })
        )
    }

private fun bridgeLabel(context: android.content.Context, bridge: LanBridge): String =
    "${bridge.name}  ·  " + context.getString(R.string.host_lan_members, bridge.members)

@Composable
private fun ForwardRow(
    entry: ForwardStatus,
    sort: ForwardSort,
    onDelete: () -> Unit,
    onEdit: () -> Unit,
) {
    val context = LocalContext.current
    val rule = entry.rule
    /* What the list is sorted by is what the eye is scanning for, so that
     * field leads the row. The note names the service when there is one;
     * otherwise the other end of the rule keeps the line useful. */
    val title = when (sort) {
        ForwardSort.PORT -> rule.hostLabel
        ForwardSort.CONTAINER -> rule.container
    }
    val beside = when {
        rule.note.isNotBlank() -> rule.note
        sort == ForwardSort.PORT -> rule.container
        else -> rule.hostLabel
    }
    val (label, color) = when (entry.state) {
        ForwardState.ACTIVE ->
            context.getString(R.string.forward_state_active) to MaterialTheme.colorScheme.primary
        ForwardState.INACTIVE ->
            context.getString(R.string.forward_state_inactive) to MaterialTheme.colorScheme.error
        ForwardState.DRIFTED ->
            context.getString(R.string.forward_state_drifted) to MaterialTheme.colorScheme.error
        ForwardState.STOPPED ->
            context.getString(R.string.forward_state_stopped) to
                MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
    }
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer,
        tonalElevation = 0.dp
    ) {
        Row(
            modifier = Modifier.padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.Top
        ) {
            /* Every line is capped at one line: a port span, a long container
             * name and a stopped state together used to wrap into a stack of
             * half-lines that was hard to scan. */
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Text(
                        title,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1
                    )
                    Text("·", style = MaterialTheme.typography.bodyMedium, color = secondary)
                    Text(
                        beside,
                        style = MaterialTheme.typography.bodyMedium,
                        color = secondary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                }
                Text(
                    "${rule.hostLabel} → ${rule.targetLabel}",
                    style = MaterialTheme.typography.bodySmall,
                    color = secondary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                Text(
                    "${rule.proto}  ·  ${rule.containerIp}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(label, style = MaterialTheme.typography.labelSmall, color = color, maxLines = 1)
                Spacer(Modifier.height(4.dp))
                Row {
                    IconButton(onClick = onEdit, modifier = Modifier.size(40.dp)) {
                        Icon(
                            Icons.Default.Edit,
                            contentDescription = context.getString(R.string.forward_edit),
                            modifier = Modifier.size(18.dp),
                            tint = secondary
                        )
                    }
                    IconButton(onClick = onDelete, modifier = Modifier.size(40.dp)) {
                        Icon(
                            Icons.Default.Delete,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp),
                            tint = MaterialTheme.colorScheme.error
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ForwardDialog(
    containers: List<RunningContainer>,
    onDismiss: () -> Unit,
    onConfirm: (HostForward) -> Unit,
    initial: HostForward? = null,
) {
    val context = LocalContext.current
    var target by remember {
        mutableStateOf(initial?.let { r -> containers.firstOrNull { it.name == r.container } }
            ?: containers.firstOrNull())
    }
    var hostPort by remember { mutableStateOf(initial?.hostLabel.orEmpty()) }
    var containerPort by remember { mutableStateOf(initial?.containerLabel.orEmpty()) }
    var proto by remember { mutableStateOf(initial?.proto ?: "tcp") }
    var note by remember { mutableStateOf(initial?.note.orEmpty()) }

    /* "8080" or "2000-2010" → (first port, how many), null when it is neither.
     * A span is capped where the field stops being readable; one rule per port
     * means a span of a thousand would file a thousand iptables calls. */
    fun parseSpan(v: String): Pair<Int, Int>? {
        val parts = v.split('-', limit = 2)
        val first = parts[0].toIntOrNull() ?: return null
        val last = parts.getOrNull(1)?.toIntOrNull() ?: first
        if (first !in 1..65535 || last !in 1..65535 || last < first || last - first > 127)
            return null
        return first to (last - first + 1)
    }

    val hostSpan = parseSpan(hostPort)
    val targetSpan = parseSpan(containerPort)
    /* Both sides move together, one container port per host port. A single
     * container port under a span is the base of the target span; a target
     * span must be exactly as long as the host one. */
    val portsOk = hostSpan != null && (
        containerPort.isBlank() || targetSpan != null &&
            (targetSpan.second == 1 || targetSpan.second == hostSpan.second)
        )
    val spanMismatch = hostSpan != null && targetSpan != null &&
        targetSpan.second > 1 && targetSpan.second != hostSpan.second
    val canAdd = target != null && target?.ip?.isNotBlank() == true && portsOk

    DsDialog(
        onDismiss = onDismiss,
        footer = {
            DialogFooterRow(
                dismissLabel = context.getString(R.string.cancel),
                confirmLabel = context.getString(
                    if (initial == null) R.string.add else R.string.save_changes
                ),
                onDismiss = onDismiss,
                onConfirm = {
                    val t = target ?: return@DialogFooterRow
                    val host = hostSpan ?: return@DialogFooterRow
                    val targetPorts = when {
                        containerPort.isBlank() -> host
                        else -> targetSpan ?: return@DialogFooterRow
                    }
                    onConfirm(
                        HostForward(
                            proto = proto,
                            hostPort = host.first,
                            container = t.name,
                            containerIp = t.ip,
                            containerPort = targetPorts.first,
                            count = host.second,
                            note = note,
                        )
                    )
                },
                confirmEnabled = canAdd
            )
        }
    ) {
        Text(
            context.getString(
                if (initial == null) R.string.port_forward_add else R.string.forward_edit
            ),
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.titleLarge
        )
        Text(
            context.getString(
                if (initial == null) R.string.port_forward_add_intro else R.string.forward_edit_intro
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        if (containers.isEmpty()) {
            Text(
                context.getString(R.string.port_forward_no_container),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error
            )
            return@DsDialog
        }

        // A dropdown rather than a list of rows: the list grew with every
        // container and pushed the port fields off the dialog.
        DsDropdown(
            label = context.getString(R.string.port_forward_container),
            selected = target ?: containers.first(),
            options = containers,
            displayName = { c ->
                if (c.ip.isBlank()) c.name
                else "${c.name}  ·  ${c.ip}"
            },
            onSelect = { target = it }
        )

        OutlinedTextField(
            value = hostPort,
            onValueChange = { text ->
                if (text.length <= 11 && text.count { it == '-' } <= 1 &&
                    text.all { it.isDigit() || it == '-' })
                    hostPort = text
            },
            label = { Text(context.getString(R.string.port_forward_host_port)) },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            colors = DsTextFieldDefaults.colors(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        OutlinedTextField(
            value = containerPort,
            onValueChange = { text ->
                if (text.length <= 11 && text.count { it == '-' } <= 1 &&
                    text.all { it.isDigit() || it == '-' })
                    containerPort = text
            },
            label = { Text(context.getString(R.string.port_forward_container_port)) },
            placeholder = { Text(hostPort) },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            colors = DsTextFieldDefaults.colors(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )

        NoteField(note) { note = it }

        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("tcp", "udp").forEach { p ->
                val selected = proto == p
                Surface(
                    onClick = { proto = p },
                    shape = MaterialTheme.shapes.small,
                    color = if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.15f)
                            else MaterialTheme.colorScheme.surfaceContainer
                ) {
                    Text(
                        p.uppercase(),
                        style = MaterialTheme.typography.labelLarge,
                        color = if (selected) MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 8.dp)
                    )
                }
            }
        }
        if (hostPort.isNotEmpty() && !portsOk) {
            Text(
                context.getString(
                    if (spanMismatch) R.string.port_forward_span_mismatch
                    else R.string.port_forward_bad_port
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
    }
}

@Composable
private fun NoteField(note: String, onNoteChange: (String) -> Unit) {
    val context = LocalContext.current
    OutlinedTextField(
        value = note,
        onValueChange = { text ->
            if (text.length <= 80 && '|' !in text && '\n' !in text)
                onNoteChange(text)
        },
        label = { Text(context.getString(R.string.port_forward_note)) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        colors = DsTextFieldDefaults.colors()
    )
}
