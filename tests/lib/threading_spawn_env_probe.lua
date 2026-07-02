local threading = require("threading")

return {
  require_type = type(require),
  package_type = type(package),
  current_type = type(threading.current)
}
