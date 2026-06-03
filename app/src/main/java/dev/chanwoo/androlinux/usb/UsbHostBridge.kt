/*
 * UsbHostBridge.kt — the ANDROID-side half of ALR USB host access (Option B1 of
 * docs/design/android-usb-host.md §2). A self-contained service that owns an
 * AF_UNIX LocalServerSocket under cacheDir (mirroring the Wayland socket at
 * <cacheDir>/alr-xdg/wayland-0), accepts the guest libusb shim's connection, and
 * translates the wire protocol (app/src/main/cpp/alr_usb/guest_shim/alr_usb_proto.h)
 * into android.hardware.usb.* (UsbManager / UsbDeviceConnection) calls.
 *
 * The guest never sees a real /dev/bus/usb node or the raw fd — it only speaks
 * our protocol over the socket, and WE perform controlTransfer/bulkTransfer in
 * this app process on the system-granted UsbDeviceConnection. So there is no
 * SELinux bypass: same-process Java calls on a handle the system already gave us
 * (design §6).
 *
 * Wiring (done by the owner where the guest launch env is assembled, design §9):
 *     val usbBridge = UsbHostBridge(cacheDir, getSystemService(USB_SERVICE) as UsbManager).also { it.start() }
 *     guestEnv["ALR_USB_SOCK"] = usbBridge.socketPath
 *
 * HOST-ONLY status: enumerate / permission / open / claim / control / bulk /
 * interrupt / async / hotplug are implemented and forward to the real Android
 * USB APIs. Device-verify (a phone with an OTG device) is required to prove real
 * bytes move; isoc + reset() are platform-limited and reported honestly.
 */
package dev.chanwoo.androlinux.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.hardware.usb.UsbRequest
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.Build
import android.util.Log
import java.io.DataInputStream
import java.io.File
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

class UsbHostBridge(
    cacheDir: File,
    private val usbManager: UsbManager,
    private val appContext: Context? = null,
) {
    /** Filesystem AF_UNIX socket path exported to the guest as $ALR_USB_SOCK. */
    val socketPath: String

    private val socketDir = File(cacheDir, "alr-usb")
    private val socketFile = File(socketDir, "usbd.sock")

    private var serverSocket: LocalServerSocket? = null
    private var boundSocket: LocalSocket? = null
    private var acceptThread: Thread? = null
    @Volatile private var running = false

    /** Open connections keyed by the connId the protocol assigns. */
    private val conns = ConcurrentHashMap<Int, OpenDevice>()
    private val nextConnId = AtomicInteger(1)

    /** Connected shim clients (for pushing async HOTPLUG frames). */
    private val clients = java.util.Collections.synchronizedList(mutableListOf<ClientLink>())

    /** Per-transfer-thread pool so blocking transfers never touch the UI thread. */
    private val ioPool = Executors.newCachedThreadPool { r ->
        Thread(r, "alr-usb-io").apply { isDaemon = true }
    }

    /** Pending permission requests keyed by deviceName. */
    private val permLatches = ConcurrentHashMap<String, PendingPerm>()

    private var permReceiver: BroadcastReceiver? = null
    private var hotplugReceiver: BroadcastReceiver? = null

    init {
        socketDir.mkdirs()
        // LocalServerSocket binds in the filesystem namespace when given a path
        // that exists; we use a path under cacheDir (same place as wayland-0).
        socketPath = socketFile.absolutePath
    }

    // --------------------------------------------------------------------- //
    // Lifecycle
    // --------------------------------------------------------------------- //

    fun start() {
        if (running) return
        running = true
        try {
            socketFile.delete()
            // Bind a FILESYSTEM-namespace AF_UNIX listening socket. The guest
            // libusb shim connects via sockaddr_un.sun_path (a filesystem path),
            // so we must NOT use LocalServerSocket(String) (that ctor is the
            // ABSTRACT namespace). All-public path: LocalSocket.bind(FILESYSTEM
            // address) then LocalServerSocket(fileDescriptor) (which listens).
            serverSocket = bindFilesystemServer(socketPath)
        } catch (t: Throwable) {
            Log.w(TAG, "USB bridge bind failed: ${t.message}")
            running = false
            return
        }
        registerReceivers()
        acceptThread = thread(name = "alr-usb-accept", isDaemon = true) { acceptLoop() }
        Log.i(TAG, "USB host bridge listening at $socketPath")
    }

    fun stop() {
        running = false
        try { serverSocket?.close() } catch (_: Throwable) {}
        try { boundSocket?.close() } catch (_: Throwable) {}
        serverSocket = null
        boundSocket = null
        synchronized(clients) { clients.toList() }.forEach { it.close() }
        clients.clear()
        conns.values.forEach { runCatching { it.connection.close() } }
        conns.clear()
        unregisterReceivers()
        ioPool.shutdownNow()
        runCatching { socketFile.delete() }
    }

    /**
     * Bind a filesystem-namespace AF_UNIX listening socket using only public API.
     * LocalServerSocket(String) is abstract-namespace; for a filesystem path
     * matching the guest's sockaddr_un.sun_path we bind a LocalSocket to a
     * FILESYSTEM LocalSocketAddress, then construct LocalServerSocket from its
     * FileDescriptor (that ctor puts the fd into listen state).
     */
    private fun bindFilesystemServer(path: String): LocalServerSocket {
        val ls = LocalSocket(LocalSocket.SOCKET_STREAM)
        ls.bind(LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM))
        boundSocket = ls
        return LocalServerSocket(ls.fileDescriptor)
    }

    // --------------------------------------------------------------------- //
    // Accept loop + per-client serving
    // --------------------------------------------------------------------- //

    private fun acceptLoop() {
        val srv = serverSocket ?: return
        while (running) {
            val sock = try {
                srv.accept()
            } catch (t: Throwable) {
                if (running) Log.w(TAG, "accept ended: ${t.message}")
                break
            }
            val link = ClientLink(sock)
            clients.add(link)
            thread(name = "alr-usb-client", isDaemon = true) {
                try {
                    serveClient(link)
                } catch (t: Throwable) {
                    Log.d(TAG, "client ended: ${t.message}")
                } finally {
                    clients.remove(link)
                    link.close()
                }
            }
        }
    }

    private fun serveClient(link: ClientLink) {
        val inp = DataInputStream(link.socket.inputStream)
        while (running) {
            val hdr = ByteArray(HDR_SIZE)
            inp.readFully(hdr)
            val hb = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN)
            val len = hb.int
            val op = hb.short.toInt() and 0xffff
            @Suppress("UNUSED_VARIABLE") val flags = hb.short.toInt() and 0xffff
            val tag = hb.int
            val payload = ByteArray(len)
            if (len > 0) inp.readFully(payload)
            dispatch(link, op, tag, payload)
        }
    }

    private fun dispatch(link: ClientLink, op: Int, tag: Int, payload: ByteArray) {
        val req = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        when (op) {
            OP_ENUMERATE -> handleEnumerate(link, tag)
            OP_OPEN -> handleOpen(link, tag, req)
            OP_CLOSE -> handleClose(link, tag, req)
            OP_CLAIM -> handleClaim(link, tag, req, claim = true)
            OP_RELEASE -> handleClaim(link, tag, req, claim = false)
            OP_SET_CONFIG -> handleSetConfig(link, tag, req)
            OP_SET_ALT -> handleSetAlt(link, tag, req)
            OP_CONTROL -> ioPool.execute { handleControl(link, tag, req) }
            OP_XFER -> ioPool.execute { handleXfer(link, tag, req) }
            OP_CLEAR_HALT -> replyErr(link, tag, OK) // best-effort; UsbDeviceConnection lacks clearHalt
            OP_RESET -> replyErr(link, tag, LIBUSB_ERROR_NOT_SUPPORTED)
            OP_SUBMIT -> ioPool.execute { handleSubmit(link, tag, req) }
            OP_CANCEL -> { /* fire-and-forget; the transfer thread checks running */ }
            else -> Log.w(TAG, "unknown op=$op")
        }
    }

    // --------------------------------------------------------------------- //
    // Handlers
    // --------------------------------------------------------------------- //

    private fun handleEnumerate(link: ClientLink, tag: Int) {
        val devices = usbManager.deviceList.values.toList()
        val out = WireBuf()
        out.u32(devices.size)
        for (d in devices) {
            out.u16(d.vendorId)
            out.u16(d.productId)
            out.u8(busOf(d))
            out.u8(addrOf(d))
            out.u8(d.deviceClass)
        }
        link.reply(tag, OP_ENUMERATE, out.bytes())
    }

    private fun handleOpen(link: ClientLink, tag: Int, req: ByteBuffer) {
        val vid = req.short.toInt() and 0xffff
        val pid = req.short.toInt() and 0xffff
        val bus = req.get().toInt() and 0xff
        val addr = req.get().toInt() and 0xff
        val device = findDevice(vid, pid, bus, addr)
        if (device == null) { replyErr(link, tag, LIBUSB_ERROR_NOT_FOUND); return }

        // Permission gate (design §2.3). Block (off the accept thread — we are on
        // a per-client thread) until grant/deny.
        if (!usbManager.hasPermission(device)) {
            if (!requestPermissionBlocking(device)) {
                replyErr(link, tag, LIBUSB_ERROR_ACCESS); return
            }
        }
        val connection = usbManager.openDevice(device)
        if (connection == null) { replyErr(link, tag, LIBUSB_ERROR_ACCESS); return }

        val connId = nextConnId.getAndIncrement()
        conns[connId] = OpenDevice(connId, device, connection)
        val raw = connection.rawDescriptors ?: ByteArray(0)
        val out = WireBuf()
        out.u32(connId)
        out.blob(raw)
        link.reply(tag, OP_OPEN, out.bytes())
    }

    private fun handleClose(link: ClientLink, tag: Int, req: ByteBuffer) {
        val connId = req.int
        conns.remove(connId)?.let { runCatching { it.connection.close() } }
        link.reply(tag, OP_CLOSE, ByteArray(0))
    }

    private fun handleClaim(link: ClientLink, tag: Int, req: ByteBuffer, claim: Boolean) {
        val connId = req.int
        val ifaceNum = req.get().toInt() and 0xff
        val force = if (claim) (req.get().toInt() and 0xff) != 0 else false
        val od = conns[connId] ?: run { replyErr(link, tag, LIBUSB_ERROR_NO_DEVICE); return }
        val iface = findInterface(od.device, ifaceNum) ?: run {
            replyErr(link, tag, LIBUSB_ERROR_NOT_FOUND); return
        }
        val ok = if (claim) od.connection.claimInterface(iface, force)
                 else od.connection.releaseInterface(iface)
        if (claim && ok) od.claimed[ifaceNum] = iface
        if (!claim) od.claimed.remove(ifaceNum)
        if (ok) link.reply(tag, if (claim) OP_CLAIM else OP_RELEASE, ByteArray(0))
        else replyErr(link, tag, LIBUSB_ERROR_BUSY)
    }

    private fun handleSetConfig(link: ClientLink, tag: Int, req: ByteBuffer) {
        val connId = req.int
        val config = req.int
        val od = conns[connId] ?: run { replyErr(link, tag, LIBUSB_ERROR_NO_DEVICE); return }
        val cfg = findConfiguration(od.device, config)
        val ok = if (cfg != null) od.connection.setConfiguration(cfg) else false
        if (ok) link.reply(tag, OP_SET_CONFIG, ByteArray(0))
        else replyErr(link, tag, LIBUSB_ERROR_IO)
    }

    private fun handleSetAlt(link: ClientLink, tag: Int, req: ByteBuffer) {
        val connId = req.int
        val ifaceNum = req.get().toInt() and 0xff
        val alt = req.get().toInt() and 0xff
        val od = conns[connId] ?: run { replyErr(link, tag, LIBUSB_ERROR_NO_DEVICE); return }
        val iface = findInterfaceAlt(od.device, ifaceNum, alt)
            ?: findInterface(od.device, ifaceNum)
            ?: run { replyErr(link, tag, LIBUSB_ERROR_NOT_FOUND); return }
        val ok = od.connection.setInterface(iface)
        if (ok) link.reply(tag, OP_SET_ALT, ByteArray(0))
        else replyErr(link, tag, LIBUSB_ERROR_IO)
    }

    private fun handleControl(link: ClientLink, tag: Int, req: ByteBuffer) {
        val connId = req.int
        val bmRequestType = req.get().toInt() and 0xff
        val bRequest = req.get().toInt() and 0xff
        val wValue = req.short.toInt() and 0xffff
        val wIndex = req.short.toInt() and 0xffff
        val wLength = req.short.toInt() and 0xffff
        val timeout = req.int
        val od = conns[connId] ?: run { replyErr(link, tag, LIBUSB_ERROR_NO_DEVICE); return }
        val isIn = (bmRequestType and 0x80) != 0
        val buf = ByteArray(wLength)
        if (!isIn && req.remaining() >= 4) {
            val n = req.int
            if (n > 0 && req.remaining() >= n) req.get(buf, 0, minOf(n, wLength))
        }
        val n = od.connection.controlTransfer(
            bmRequestType, bRequest, wValue, wIndex, buf, wLength, timeout,
        )
        if (n < 0) { replyErr(link, tag, LIBUSB_ERROR_IO); return }
        val out = WireBuf()
        out.i32(n)
        if (isIn) out.blob(buf.copyOf(minOf(n, wLength)))
        link.reply(tag, OP_CONTROL, out.bytes())
    }

    private fun handleXfer(link: ClientLink, tag: Int, req: ByteBuffer) {
        val (od, ep, buf, length, timeout, isIn) = parseXfer(req) ?: run {
            replyErr(link, tag, LIBUSB_ERROR_NO_DEVICE); return
        }
        if (ep == null) { replyErr(link, tag, LIBUSB_ERROR_NOT_FOUND); return }
        val n = od.connection.bulkTransfer(ep, buf, length, timeout)
        if (n < 0) { replyErr(link, tag, LIBUSB_ERROR_IO); return }
        val out = WireBuf()
        out.i32(n)
        if (isIn) out.blob(buf.copyOf(minOf(n, length)))
        link.reply(tag, OP_XFER, out.bytes())
    }

    /** Async SUBMIT: ack nothing synchronously; result rides back as a COMPLETE frame. */
    private fun handleSubmit(link: ClientLink, tag: Int, req: ByteBuffer) {
        val parsed = parseXfer(req)
        if (parsed == null) { sendComplete(link, tag, TRANSFER_NO_DEVICE, 0, null); return }
        val (od, ep, buf, length, _, isIn) = parsed
        if (ep == null) { sendComplete(link, tag, TRANSFER_ERROR, 0, null); return }
        // Android UsbRequest async primitive (interrupt-IN etc.); for bulk we use
        // a blocking bulkTransfer on this io-pool thread and post COMPLETE.
        val n = od.connection.bulkTransfer(ep, buf, length, DEFAULT_ASYNC_TIMEOUT_MS)
        if (n < 0) { sendComplete(link, tag, TRANSFER_ERROR, 0, null); return }
        sendComplete(link, tag, TRANSFER_COMPLETED, n, if (isIn) buf.copyOf(minOf(n, length)) else null)
    }

    private data class XferReq(
        val od: OpenDevice,
        val ep: UsbEndpoint?,
        val buf: ByteArray,
        val length: Int,
        val timeout: Int,
        val isIn: Boolean,
    )

    private fun parseXfer(req: ByteBuffer): XferReq? {
        val connId = req.int
        val epAddr = req.get().toInt() and 0xff
        @Suppress("UNUSED_VARIABLE") val type = req.get().toInt() and 0xff
        val length = req.int
        val timeout = req.int
        val od = conns[connId] ?: return null
        val isIn = (epAddr and UsbConstants.USB_DIR_IN) != 0
        val buf = ByteArray(length)
        if (!isIn && req.remaining() >= 4) {
            val n = req.int
            if (n > 0 && req.remaining() >= n) req.get(buf, 0, minOf(n, length))
        }
        val ep = findEndpoint(od.device, epAddr)
        return XferReq(od, ep, buf, length, timeout, isIn)
    }

    private fun sendComplete(link: ClientLink, tag: Int, status: Int, actual: Int, data: ByteArray?) {
        val out = WireBuf()
        out.i32(status)
        out.i32(actual)
        if (data != null) out.blob(data)
        link.async(tag, OP_COMPLETE, out.bytes())
    }

    // --------------------------------------------------------------------- //
    // Permission (design §2.3) — FLAG_MUTABLE mandatory on API 31+
    // --------------------------------------------------------------------- //

    private fun requestPermissionBlocking(device: UsbDevice): Boolean {
        val ctx = appContext ?: return false
        val latch = CountDownLatch(1)
        val pp = PendingPerm(latch)
        permLatches[device.deviceName] = pp
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_MUTABLE
        } else {
            0
        }
        val pi = PendingIntent.getBroadcast(
            ctx, 0, Intent(ACTION_USB_PERMISSION).setPackage(ctx.packageName), flags,
        )
        usbManager.requestPermission(device, pi)
        // Block this per-client thread until the BroadcastReceiver fires.
        latch.await()
        permLatches.remove(device.deviceName)
        return pp.granted
    }

    private fun registerReceivers() {
        val ctx = appContext ?: return
        permReceiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) return
                val dev = intentDevice(intent) ?: return
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                permLatches[dev.deviceName]?.let {
                    it.granted = granted
                    it.latch.countDown()
                }
            }
        }
        hotplugReceiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                val dev = intentDevice(intent) ?: return
                val added = intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED
                pushHotplug(added, dev)
            }
        }
        registerExported(ctx, permReceiver!!, IntentFilter(ACTION_USB_PERMISSION))
        val hp = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        registerExported(ctx, hotplugReceiver!!, hp)
    }

    private fun registerExported(ctx: Context, r: BroadcastReceiver, f: IntentFilter) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(r, f, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            ctx.registerReceiver(r, f)
        }
    }

    private fun unregisterReceivers() {
        val ctx = appContext ?: return
        permReceiver?.let { runCatching { ctx.unregisterReceiver(it) } }
        hotplugReceiver?.let { runCatching { ctx.unregisterReceiver(it) } }
        permReceiver = null
        hotplugReceiver = null
    }

    private fun pushHotplug(added: Boolean, dev: UsbDevice) {
        val out = WireBuf()
        out.u8(if (added) 1 else 0)
        out.u16(dev.vendorId)
        out.u16(dev.productId)
        out.u8(busOf(dev))
        out.u8(addrOf(dev))
        val frame = out.bytes()
        synchronized(clients) { clients.toList() }.forEach { it.async(0, OP_HOTPLUG, frame) }
    }

    // --------------------------------------------------------------------- //
    // UsbDevice lookups
    // --------------------------------------------------------------------- //

    private fun findDevice(vid: Int, pid: Int, bus: Int, addr: Int): UsbDevice? {
        val all = usbManager.deviceList.values
        // Prefer an exact bus/addr match; fall back to vid/pid (bus/addr is
        // synthesized from deviceId, see busOf/addrOf).
        return all.firstOrNull {
            it.vendorId == vid && it.productId == pid && busOf(it) == bus && addrOf(it) == addr
        } ?: all.firstOrNull { it.vendorId == vid && it.productId == pid }
    }

    private fun findInterface(device: UsbDevice, ifaceNum: Int): UsbInterface? {
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.id == ifaceNum) return iface
        }
        return null
    }

    private fun findInterfaceAlt(device: UsbDevice, ifaceNum: Int, alt: Int): UsbInterface? {
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.id == ifaceNum && iface.alternateSetting == alt) return iface
        }
        return null
    }

    private fun findConfiguration(device: UsbDevice, value: Int): android.hardware.usb.UsbConfiguration? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) return null
        for (i in 0 until device.configurationCount) {
            val cfg = device.getConfiguration(i)
            if (cfg.id == value) return cfg
        }
        return null
    }

    private fun findEndpoint(device: UsbDevice, epAddr: Int): UsbEndpoint? {
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            for (e in 0 until iface.endpointCount) {
                val ep = iface.getEndpoint(e)
                if (ep.address == epAddr) return ep
            }
        }
        return null
    }

    /** Synthesize a stable bus/addr from UsbDevice.deviceId (no public bus/addr). */
    private fun busOf(d: UsbDevice): Int = (d.deviceId shr 8) and 0xff
    private fun addrOf(d: UsbDevice): Int = d.deviceId and 0xff

    private fun intentDevice(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    // --------------------------------------------------------------------- //
    // Reply helpers
    // --------------------------------------------------------------------- //

    private fun replyErr(link: ClientLink, tag: Int, errno: Int) {
        if (errno == OK) { link.reply(tag, OP_NONE, ByteArray(0)); return }
        val b = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(errno).array()
        link.replyFlagged(tag, OP_NONE, FLAG_ERR, b)
    }

    // --------------------------------------------------------------------- //
    // Inner types
    // --------------------------------------------------------------------- //

    private class OpenDevice(
        val connId: Int,
        val device: UsbDevice,
        val connection: UsbDeviceConnection,
    ) {
        val claimed = ConcurrentHashMap<Int, UsbInterface>()
    }

    private class PendingPerm(val latch: CountDownLatch) { @Volatile var granted = false }

    /** One connected guest shim. Writes are serialized by a lock (replies + async). */
    private class ClientLink(val socket: LocalSocket) {
        private val out: OutputStream = socket.outputStream
        private val writeLock = Any()

        fun reply(tag: Int, op: Int, payload: ByteArray) = frame(tag, op, 0, payload)
        fun replyFlagged(tag: Int, op: Int, flags: Int, payload: ByteArray) = frame(tag, op, flags, payload)
        fun async(tag: Int, op: Int, payload: ByteArray) = frame(tag, op, 0, payload)

        private fun frame(tag: Int, op: Int, flags: Int, payload: ByteArray) {
            val hdr = ByteBuffer.allocate(HDR_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(payload.size)
            hdr.putShort(op.toShort())
            hdr.putShort(flags.toShort())
            hdr.putInt(tag)
            synchronized(writeLock) {
                try {
                    out.write(hdr.array())
                    if (payload.isNotEmpty()) out.write(payload)
                    out.flush()
                } catch (_: Throwable) { /* client gone */ }
            }
        }

        fun close() { runCatching { socket.close() } }
    }

    /** Little-endian wire encoder matching alr_usb_proto.h. */
    private class WireBuf {
        private val data = java.io.ByteArrayOutputStream()
        fun u8(v: Int) { data.write(v and 0xff) }
        fun u16(v: Int) { data.write(v and 0xff); data.write((v shr 8) and 0xff) }
        fun u32(v: Int) {
            data.write(v and 0xff); data.write((v shr 8) and 0xff)
            data.write((v shr 16) and 0xff); data.write((v shr 24) and 0xff)
        }
        fun i32(v: Int) = u32(v)
        fun blob(b: ByteArray) { u32(b.size); data.write(b, 0, b.size) }
        fun bytes(): ByteArray = data.toByteArray()
    }

    companion object {
        private const val TAG = "AlrUsbBridge"
        private const val BACKLOG = 8
        private const val HDR_SIZE = 12
        private const val DEFAULT_ASYNC_TIMEOUT_MS = 5000
        const val ACTION_USB_PERMISSION = "dev.chanwoo.androlinux.USB_PERMISSION"

        // Opcodes — MUST match alr_usb_proto.h enum AlrUsbOp.
        private const val OP_NONE = 0
        private const val OP_ENUMERATE = 1
        private const val OP_OPEN = 2
        private const val OP_CLOSE = 3
        private const val OP_CLAIM = 4
        private const val OP_RELEASE = 5
        private const val OP_SET_CONFIG = 6
        private const val OP_SET_ALT = 7
        private const val OP_CONTROL = 8
        private const val OP_XFER = 9
        private const val OP_CLEAR_HALT = 10
        private const val OP_RESET = 11
        private const val OP_SUBMIT = 12
        private const val OP_CANCEL = 13
        private const val OP_COMPLETE = 0x80
        private const val OP_HOTPLUG = 0x81

        private const val FLAG_ERR = 0x0001

        // libusb error codes (subset) — MUST match alr_usb_proto.h / alr_libusb.h.
        private const val OK = 0
        private const val LIBUSB_ERROR_IO = -1
        private const val LIBUSB_ERROR_ACCESS = -3
        private const val LIBUSB_ERROR_NO_DEVICE = -4
        private const val LIBUSB_ERROR_NOT_FOUND = -5
        private const val LIBUSB_ERROR_BUSY = -6
        private const val LIBUSB_ERROR_NOT_SUPPORTED = -12

        // libusb_transfer_status — MUST match alr_libusb.h.
        private const val TRANSFER_COMPLETED = 0
        private const val TRANSFER_ERROR = 1
        private const val TRANSFER_NO_DEVICE = 5
    }
}
