// The ring list: which rings this phone knows, which are connected, and which
// one the screen is showing.
//
// **Those last two are different questions, and the UI keeps them apart.** The
// switch on each row connects that ring; every ring with it on holds its own
// live session, independently. The radio beside it only chooses whose readings
// the Live and Graphs tabs draw -- pointing the screen at one ring does not
// stop the other, which is the whole reason both can be worn at once.
//
// The row is deliberately informative rather than pretty. When two rings are
// paired and one of them is a bare bench board fabricating vitals, the thing
// that matters is being able to tell at a glance which is which -- hence the
// address and the stored-row count next to every label.

import 'package:flutter/material.dart';

import 'history_db.dart';
import 'ring_fleet.dart';

class RingSheet extends StatefulWidget {
  const RingSheet({super.key});

  @override
  State<RingSheet> createState() => _RingSheetState();
}

class _RingSheetState extends State<RingSheet> {
  List<Ring> _rings = const [];
  Map<String, int> _counts = const {};
  bool _loading = true;
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    await _fleet.refresh();
    if (!mounted) return;
    setState(() {
      _rings = _fleet.rings;
      _counts = _fleet.counts;
      _loading = false;
    });
  }

  RingFleet get _fleet => RingFleet.instance;

  // Switching tears down a live connection and brings another up, which is not
  // instant. Locking the sheet for the duration stops a second tap landing
  // mid-teardown, where the link would be half-owned by two rings.
  Future<void> _guard(Future<void> Function() action) async {
    if (_busy) return;
    setState(() => _busy = true);
    try {
      await action();
      await _reload();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _rename(Ring ring) async {
    final controller = TextEditingController(text: ring.label);
    final name = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Rename ring'),
        content: TextField(
          controller: controller,
          autofocus: true,
          decoration: const InputDecoration(
            labelText: 'Label',
            hintText: 'e.g. Bench board, or Ring on finger',
          ),
          onSubmitted: (v) => Navigator.pop(ctx, v),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, controller.text),
            child: const Text('Save'),
          ),
        ],
      ),
    );
    final trimmed = name?.trim();
    if (trimmed == null || trimmed.isEmpty) return;
    await _guard(() => _fleet.renameRing(ring.id, trimmed));
  }

  Future<void> _forget(Ring ring) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('Forget ${ring.label}?'),
        // Said plainly, because the readings are the part people expect to lose
        // and the part that is actually kept.
        content: const Text(
          'The pairing is removed and this ring stops connecting.\n\n'
          'Its stored readings are kept — they were real measurements and '
          'unpairing does not undo them.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Forget'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await _guard(() => _fleet.forgetRing(ring.id));
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Text('Rings', style: theme.textTheme.titleLarge),
                const Spacer(),
                if (_busy)
                  const SizedBox(
                      height: 18, width: 18, child: CircularProgressIndicator(strokeWidth: 2)),
              ],
            ),
            const SizedBox(height: 4),
            Text(
              'Each switch connects a ring. The dot chooses which one the Live '
              'and Graphs tabs show.',
              style: theme.textTheme.bodySmall
                  ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
            ),
            const SizedBox(height: 12),

            if (_loading)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 28),
                child: Center(child: CircularProgressIndicator()),
              )
            else if (_rings.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 28),
                child: Text(
                  'No rings paired yet. Press Pair a new ring below.',
                  textAlign: TextAlign.center,
                  style: theme.textTheme.bodyMedium
                      ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
                ),
              )
            else
              Flexible(
                child: ListView.builder(
                  shrinkWrap: true,
                  itemCount: _rings.length,
                  itemBuilder: (_, i) => _row(_rings[i], theme),
                ),
              ),

            const Divider(height: 24),
            Row(children: [
              Expanded(
                child: TextButton.icon(
                  onPressed: _busy ? null : () => _guard(_fleet.connectNew),
                  icon: const Icon(Icons.add_circle_outline),
                  label: const Text('Pair a new ring'),
                ),
              ),
              Expanded(
                child: TextButton.icon(
                  onPressed: _busy || _rings.every((r) => !r.enabled)
                      ? null
                      : () => _guard(() async {
                            for (final r in _rings.where((r) => r.enabled)) {
                              await _fleet.setEnabled(r.id, false);
                            }
                          }),
                  icon: const Icon(Icons.link_off),
                  label: const Text('Disconnect all'),
                ),
              ),
            ]),
          ],
        ),
      ),
    );
  }

  Widget _row(Ring ring, ThemeData theme) {
    final n = _counts[ring.id] ?? 0;
    final session = _fleet.session(ring.id);
    final live = session?.connected ?? false;

    return ListTile(
      contentPadding: EdgeInsets.zero,
      // Focus, not connection. Disabled for a ring that is not connected --
      // pointing the charts at a ring that is not streaming would show a frozen
      // screen with nothing to say why.
      leading: Radio<String>(
        value: ring.id,
        groupValue: _fleet.focusId,
        toggleable: false,
        onChanged: _busy || !ring.enabled
            ? null
            : (v) {
                if (v != null) _fleet.setFocus(v);
                setState(() {});
              },
      ),
      title: Row(
        children: [
          Flexible(child: Text(ring.label, overflow: TextOverflow.ellipsis)),
          const SizedBox(width: 8),
          if (live) ...[
            Icon(Icons.circle, size: 9, color: theme.colorScheme.primary),
            const SizedBox(width: 4),
            Text('live', style: theme.textTheme.labelSmall),
          ] else if (ring.enabled)
            Text('waiting', style: theme.textTheme.labelSmall),
        ],
      ),
      subtitle: Text(
        '${ring.id}\n$n reading(s) stored',
        style: theme.textTheme.bodySmall
            ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
      ),
      isThreeLine: true,
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Switch(
            value: ring.enabled,
            onChanged: _busy
                ? null
                : (v) => _guard(() => _fleet.setEnabled(ring.id, v)),
          ),
          PopupMenuButton<String>(
            enabled: !_busy,
            onSelected: (v) => v == 'rename' ? _rename(ring) : _forget(ring),
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'rename', child: Text('Rename')),
              PopupMenuItem(value: 'forget', child: Text('Forget')),
            ],
          ),
        ],
      ),
    );
  }
}
