const path = require('path')
const load = require('node-gyp-build')

module.exports = load(path.join(__dirname, '..'))
