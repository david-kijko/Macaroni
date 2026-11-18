// Browser endpoint for the MACVM SSH forwards on the owner's laptop.
export function isMacvmForward() {
    return location.origin === 'http://127.0.0.1:18991';
}

export function forwardedCandidate(candidate) {
    if (!isMacvmForward()) return candidate;
    const fields = candidate.split(' ');
    const port = Number(fields[5]);
    if (
        fields[2]?.toLowerCase() === 'tcp' &&
        fields.includes('passive') &&
        port >= 48550 &&
        port <= 48573
    ) {
        fields[4] = '127.0.0.1';
        return fields.join(' ');
    }
    return candidate;
}
