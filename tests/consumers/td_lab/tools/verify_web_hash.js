async (page, value, utilityUrl) => {
    // Shared QA hash service for the MCP runtime: no Node modules or GL.
    const url = new URL(utilityUrl);
    if (!['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname) ||
        !['http:', 'https:'].includes(url.protocol) || !url.pathname.startsWith('/__jce_qa_hash_'))
        throw new Error('Dedicated loopback QA hash URL required');
    const bytes = Buffer.isBuffer(value) ? value : Buffer.from(value, typeof value === 'string' ? 'utf8' : undefined);
    const context = await page.context().browser().newContext();
    try {
        await context.route('**/*', async route => {
            if (route.request().url() === url.href) {
                await route.fulfill({status: 200, contentType: 'text/html',
                    body: '<!doctype html><meta charset="utf-8"><title>JCE QA hash</title>'});
            } else { await route.abort('blockedbyclient'); }
        });
        const utility = await context.newPage();
        await utility.goto(url.href, {waitUntil: 'domcontentloaded'});
        return await utility.evaluate(async encoded => {
            if (!isSecureContext || !crypto.subtle) throw new Error('Secure browser WebCrypto unavailable');
            const binary = atob(encoded), input = new Uint8Array(binary.length);
            for (let i = 0; i < binary.length; ++i) input[i] = binary.charCodeAt(i);
            const digest = await crypto.subtle.digest('SHA-256', input);
            return Array.from(new Uint8Array(digest), value => value.toString(16).padStart(2, '0')).join('');
        }, bytes.toString('base64'));
    } finally { await context.close(); }
}
