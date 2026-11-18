package com.droidspaces.app.ui.screen

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
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
    var chosenBridge by remember { mutableStateOf<String?>(null) }
    var notice by remember { mutableStateOf<String?>(null) }

    suspend fun refresh() {
        val st = HostNetworkManager.status()
        status = st
        if (chosenBridge == null) chosenBridge = st?.bridges?.firstOrNull()?.name
        val lanPrefix = st?.ip?.takeIf { it.isNotBlank() }?.substringBeforeLast('.')?.plus(".")
        running = HostNetworkManager.runningContainers(lanPrefix)
        forwardingOn = HostNetworkManager.forwardingEnabled()
        forwards = HostNetworkManager.forwardStates(HostNetworkManager.listForwards(), running)
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

        when {
            forwards.isEmpty() -> Text(
                context.getString(R.string.port_forwards_empty),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            else -> forwards.forEach { entry ->
                ForwardRow(entry) {
                    scope.launch {
                        HostNetworkManager.removeForward(entry.rule)
                        refresh()
                    }
                }
            }
        }

        Surface(
            onClick = { adding = true },
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            color = MaterialTheme.colorScheme.primary.copy(alpha = 0.10f),
            tonalElevation = 0.dp
        ) {
            Row(
                modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp),
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(Modifier.width(10.dp))
                Text(
                    context.getString(R.string.port_forward_add),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        notice?.let {
            Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }

        Spacer(Modifier.height(12.dp))
    }

    if (adding) {
        AddForwardDialog(
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
    onToggle: (Boolean) -> Unit,
) {
    val context = LocalContext.current
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 0.dp
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
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
        }
    }
}

private fun bridgeLabel(context: android.content.Context, bridge: LanBridge): String =
    "${bridge.name}  ·  " + context.getString(R.string.host_lan_members, bridge.members)

@Composable
private fun ForwardRow(entry: ForwardStatus, onDelete: () -> Unit) {
    val context = LocalContext.current
    val (label, color) = when (entry.state) {
        ForwardState.ACTIVE ->
            context.getString(R.string.forward_state_active) to MaterialTheme.colorScheme.primary
        ForwardState.INACTIVE ->
            context.getString(R.string.forward_state_inactive) to MaterialTheme.colorScheme.error
        ForwardState.DRIFTED ->
            context.getString(R.string.forward_state_drifted) to MaterialTheme.colorScheme.error
        ForwardState.STOPPED ->
            context.getString(R.string.forward_state_stopped) to MaterialTheme.colorScheme.onSurfaceVariant
    }

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer,
        tonalElevation = 0.dp
    ) {
        Row(
            modifier = Modifier.padding(start = 14.dp, end = 4.dp, top = 6.dp, bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    "${entry.rule.hostPort} → ${entry.rule.container}:${entry.rule.containerPort}",
                    style = MaterialTheme.typography.bodyLarge
                )
                Text(
                    "${entry.rule.proto}  ·  ${entry.rule.containerIp}  ·  $label",
                    style = MaterialTheme.typography.bodySmall,
                    color = color
                )
            }
            IconButton(onClick = onDelete) {
                Icon(
                    Icons.Default.Delete,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp),
                    tint = MaterialTheme.colorScheme.error
                )
            }
        }
    }
}

@Composable
private fun AddForwardDialog(
    containers: List<RunningContainer>,
    onDismiss: () -> Unit,
    onConfirm: (HostForward) -> Unit,
) {
    val context = LocalContext.current
    var target by remember { mutableStateOf(containers.firstOrNull()) }
    var hostPort by remember { mutableStateOf("") }
    var containerPort by remember { mutableStateOf("") }
    var proto by remember { mutableStateOf("tcp") }

    fun validPort(v: String) = v.toIntOrNull()?.let { it in 1..65535 } == true
    val portsOk = validPort(hostPort) && (containerPort.isBlank() || validPort(containerPort))
    val canAdd = target != null && target?.ip?.isNotBlank() == true && portsOk

    DsDialog(
        onDismiss = onDismiss,
        footer = {
            DialogFooterRow(
                dismissLabel = context.getString(R.string.cancel),
                confirmLabel = context.getString(R.string.add),
                onDismiss = onDismiss,
                onConfirm = {
                    val t = target ?: return@DialogFooterRow
                    onConfirm(
                        HostForward(
                            proto = proto,
                            hostPort = hostPort.toInt(),
                            container = t.name,
                            containerIp = t.ip,
                            containerPort = containerPort.ifBlank { hostPort }.toInt()
                        )
                    )
                },
                confirmEnabled = canAdd
            )
        }
    ) {
        Text(
            context.getString(R.string.port_forward_add),
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.titleLarge
        )
        Text(
            context.getString(R.string.port_forward_add_intro),
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
            onValueChange = { if (it.length <= 5 && it.all { ch -> ch.isDigit() }) hostPort = it },
            label = { Text(context.getString(R.string.port_forward_host_port)) },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            colors = DsTextFieldDefaults.colors(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        OutlinedTextField(
            value = containerPort,
            onValueChange = { if (it.length <= 5 && it.all { ch -> ch.isDigit() }) containerPort = it },
            label = { Text(context.getString(R.string.port_forward_container_port)) },
            placeholder = { Text(hostPort) },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.medium,
            colors = DsTextFieldDefaults.colors(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )

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
                context.getString(R.string.port_forward_bad_port),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }
    }
}
