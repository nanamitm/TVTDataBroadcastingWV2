const path = require('path');
const webpack = require('webpack')

module.exports = {
    entry: {
        TVTDataBroadcastingWV2: './src/TVTDataBroadcastingWV2.ts',
        aribhtml5_runtime: './src/aribhtml5_runtime.ts',
    },
    output: {
        path: path.resolve(__dirname, 'dist'),
        filename: '[name].js',
    },
    plugins: [
        new webpack.NormalModuleReplacementPlugin(/^\.\/ait$/, resource => {
            if (resource.context.replace(/\\/g, '/').endsWith('/web-bml/dist/server')) {
                resource.request = path.resolve(__dirname, 'src/broadcast_ait.ts');
            }
        }),
    ],
    module: {
        rules: [
            {
                test: /\.ts$/,
                use: 'ts-loader',
            },
        ],
    },
    resolve: {
        // The linked web-bml submodule has its own lockfile; keep the patched
        // parser used by this plugin instead of bundling that older copy.
        alias: {
            'fast-xml-parser': path.resolve(__dirname, 'node_modules/fast-xml-parser'),
        },
        extensions: [
            '.ts', '.js',
        ],
    },
};
