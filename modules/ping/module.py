from osv.modules.api import *
from osv.modules.filemap import FileMap
from osv.modules import api
usr_files = FileMap()
usr_files.add('${OSV_BASE}/modules/ping').to('/ping')
full = api.run('/ping')
default = full
