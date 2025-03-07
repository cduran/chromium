#!/usr/bin/env python
# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

'''Creates a ADMX group policy template file from an extension schema.json file.

generate_extension_admx.py --name <name> --id <id> --schema <schema_file>
                           --admx <admx_file> --adml <adml_file>

<name> is the human-readable name of the extension.
<id> is the 32-character extension ID.
<schema_file> is the file path of the input schema.json file.
<admx_file> is the file path of the output ADMX file.
<adml_file> is the file path of the output ADML language file (e.g. in en-US).

Example:
  Download the managed bookmarks extension from
    http://developer.chrome.com/extensions/examples/extensions/managed_bookmarks.zip
  to obtain the schema.json file.

  generate_extension_admx.py --name 'Managed Bookmarks'
                             --id 'gihmafigllmhbppdfjnfecimiohcljba'
                             --schema '/path/to/schema.json'
                             --admx '/path/to/managed_bookmarks.admx'
                             --adml '/path/to/en-US/managed_bookmarks.adml'

'''

import re
import sys

from optparse import OptionParser
from xml.dom import minidom

class AdmxGenerator(object):
  '''Generates ADMX and ADML templates'''

  def __init__(self, extension_name, extension_id, schema):
    self._extension_name = extension_name
    self._extension_id = extension_id
    self._schema = schema
    self._schema_map = {}
    self._strings_seen = {}
    self._admx_doc = None
    self._adml_doc = None
    self._policies_elem = None
    self._string_table_elem = None
    self._presentation_table_elem = None

    # Registry key for policies. Treat all policies as mandatory. Recommended
    # policies would use 'Recommended' instead of 'Policy'.
    self._REGISTRY_KEY = \
        'Software\\Policies\\Google\\Chrome\\3rdparty\\extensions\\' + \
        extension_id + '\\Policy'

  def CreateTemplateXml(self):
    '''
    Creates ADMX and ADML templates.

    @return (ADMX xml, ADML xml) tuple.

    '''

    # ADML must be first as ADMX uses the ADML doc to write strings.
    self._BeginAdmlTemplate()
    self._BeginAdmxTemplate()

    properties = self._schema['properties']
    for policy_name, policy_schema in properties.items():
      self._AddPolicy(policy_name, policy_schema)

    return self._ToPrettyXml(self._admx_doc.toxml()), \
           self._adml_doc.toxml()


  def _AddElement(self, parent, name):
    '''
    Adds an element named |name| as child of |parent|.

    @return The new XML element.

    '''
    doc = parent.ownerDocument
    element = doc.createElement(name)
    parent.appendChild(element)
    return element


  def _SetAttribute(self, elem, name, value, string_id = None):
    '''
    Sets the attribute |name| = |value| on the element |elem|. If |string_id|
    is given, a new string with that ID is added to the strings table in the
    ADML file.

    '''
    string_id = self._ToId(string_id)
    if (string_id):
      elem.setAttribute(name, '$(string.%s)' % string_id)
      self._AddString(string_id, value)
    else:
      elem.setAttribute(name, value)


  def _ToId(self, id_str):
    '''
    Replaces all non-alphanumeric characters by underscores.

    '''
    return re.sub('[^0-9a-zA-Z]+', '_', id_str) if id_str else None


  def _AddString(self, string_id, text):
    '''
    Adds a string with ID |string_id| to the strings table in the ADML doc or
    reuses an existing string.

    '''
    string_id = self._ToId(string_id)
    if string_id in self._strings_seen:
      assert text == self._strings_seen[string_id]
    else:
      self._strings_seen[string_id] = text
      string_elem = self._AddElement(self._string_table_elem, 'string')
      self._SetAttribute(string_elem, 'id', string_id)
      string_elem.appendChild(self._adml_doc.createTextNode(text))


  def _AddNamespace(self, namespaces_elem, elem_name, namespace, prefix):
    '''
    Adds an ADMX namespace node.

    '''
    namespace_elem = self._AddElement(namespaces_elem, elem_name)
    self._SetAttribute(namespace_elem, 'namespace', namespace)
    self._SetAttribute(namespace_elem, 'prefix', prefix)


  def _AddCategory(self, categories_elem, display_name, name, parent_category):
    '''
    Adds an ADMX category.

    '''
    category_elem = self._AddElement(categories_elem, 'category')
    self._SetAttribute(category_elem, 'displayName', display_name, name)
    self._SetAttribute(category_elem, 'name', name)
    parent_category_elem = self._AddElement(category_elem, 'parentCategory')
    self._SetAttribute(parent_category_elem, 'ref', parent_category)


  def _BeginAdmlTemplate(self):
    '''
    Writes the header of the ADML doc.

    '''
    dom_impl = minidom.getDOMImplementation('')
    self._adml_doc = dom_impl.createDocument(None, 'policyDefinitionResources',
                                             None)
    root_elem = self._adml_doc.documentElement
    self._SetAttribute(root_elem, 'revision', '1.0')
    self._SetAttribute(root_elem, 'schemaVersion', '1.0')

    self._AddElement(root_elem, 'displayName')
    self._AddElement(root_elem, 'description')
    resources_elem = self._AddElement(root_elem, 'resources')
    self._string_table_elem = self._AddElement(resources_elem, 'stringTable')
    self._presentation_table_elem = self._AddElement(resources_elem,
                                                    'presentationTable')


  def _BeginAdmxTemplate(self):
    '''
    Writes the header of the ADMX doc.

    '''
    dom_impl = minidom.getDOMImplementation('')
    self._admx_doc = dom_impl.createDocument(None, 'policyDefinitions', None)
    root_elem = self._admx_doc.documentElement
    self._SetAttribute(root_elem, 'revision', '1.0')
    self._SetAttribute(root_elem, 'schemaVersion', '1.0')

    namespaces_elem = self._AddElement(root_elem, 'policyNamespaces')
    self._AddNamespace(namespaces_elem, 'target',
                       'Google.Policies.ThirdParty.' + self._extension_id,
                       'extension')
    self._AddNamespace(namespaces_elem, 'using', 'Google.Policies', 'Google')
    self._AddNamespace(namespaces_elem, 'using', 'Microsoft.Policies.Windows',
                       'windows')

    resources_elem = self._AddElement(root_elem, 'resources')
    self._SetAttribute(resources_elem, 'minRequiredRevision', '1.0')

    supported_on_elem = self._AddElement(root_elem, 'supportedOn')
    definitions_elem = self._AddElement(supported_on_elem, 'definitions')
    definition_elem = self._AddElement(definitions_elem, 'definition')
    self._SetAttribute(definition_elem, 'displayName',
                       'Microsoft Windows 7 or later',
                       'SUPPORTED_WIN7')
    self._SetAttribute(definition_elem, 'name', 'SUPPORTED_WIN7')

    categories_elem = self._AddElement(root_elem, 'categories')
    self._AddCategory(categories_elem,
                      self._extension_name, 'extension', 'Google:Cat_Google')

    self._policies_elem = self._AddElement(root_elem, 'policies')


  def _AddPolicy(self, policy_name, policy_schema):
    '''
    Adds a policy with name |policy_name| and schema data |policy_schema| to
    the ADMX/ADML docs.

    '''
    policy_id = self._ToId(policy_name)
    policy_elem = self._AddElement(self._policies_elem, 'policy')
    policy_title = policy_schema['title']
    policy_desc = policy_schema['description']
    self._SetAttribute(policy_elem, 'name', policy_name)
    self._SetAttribute(policy_elem, 'class', 'Both')
    self._SetAttribute(policy_elem, 'displayName', policy_title,
                       policy_id)
    self._SetAttribute(policy_elem, 'explainText', policy_desc,
                       policy_id + '_Explain')
    self._SetAttribute(policy_elem, 'presentation',
                       '$(presentation.%s)' % policy_id)
    self._SetAttribute(policy_elem, 'key', self._REGISTRY_KEY)

    parent_category_elem = self._AddElement(policy_elem, 'parentCategory')
    self._SetAttribute(parent_category_elem, 'ref', 'extension')

    supported_on_elem = self._AddElement(policy_elem, 'supportedOn')
    self._SetAttribute(supported_on_elem, 'ref', 'SUPPORTED_WIN7')

    if 'id' in policy_schema:
      # Keep id map for referenced schema.
      self._schema_map[policy_schema['id']] = policy_schema
    elif ('$ref' in policy_schema):
      # Instantiate referenced schema.
      referenced_schema = self._schema_map[policy_schema['$ref']]
      for key, value in referenced_schema.iteritems():
        if not key in policy_schema:
          policy_schema[key] = value

    desc_id = policy_id + '_Part'
    presentation_elem = self._AddElement(self._presentation_table_elem,
                                        'presentation')
    self._SetAttribute(presentation_elem, 'id', policy_id)
    if (policy_schema['type'] == 'boolean'):
      self._SetAttribute(policy_elem, 'valueName', policy_id)

      enabled_value_elem = self._AddElement(policy_elem, 'enabledValue')
      decimal_elem = self._AddElement(enabled_value_elem, 'decimal')
      self._SetAttribute(decimal_elem, 'value', '1')

      disabled_value_elem = self._AddElement(policy_elem, 'disabledValue')
      decimal_elem = self._AddElement(disabled_value_elem, 'decimal')
      self._SetAttribute(decimal_elem, 'value', '0')
    elif (policy_schema['type'] == 'integer'):
      elements_elem = self._AddElement(policy_elem, 'elements')
      decimal_elem = self._AddElement(elements_elem, 'decimal')
      self._SetAttribute(decimal_elem, 'id', desc_id)
      self._SetAttribute(decimal_elem, 'valueName', policy_id)

      textbox_elem = self._AddElement(presentation_elem, 'decimalTextBox')
      self._SetAttribute(textbox_elem, 'refId', desc_id)
      textbox_elem.appendChild(self._adml_doc.createTextNode(policy_title))
    elif (policy_schema['type'] == 'string'):
      elements_elem = self._AddElement(policy_elem, 'elements')
      text_elem = self._AddElement(elements_elem, 'text')
      self._SetAttribute(text_elem, 'id', desc_id)
      self._SetAttribute(text_elem, 'valueName', policy_id)

      textbox_elem = self._AddElement(presentation_elem, 'textBox')
      self._SetAttribute(textbox_elem, 'refId', desc_id)
      label_elem = self._AddElement(textbox_elem, 'label')
      label_elem.appendChild(self._adml_doc.createTextNode(policy_title))
    elif (policy_schema['type'] == 'array'):
      elements_elem = self._AddElement(policy_elem, 'elements')
      list_elem = self._AddElement(elements_elem, 'list')
      self._SetAttribute(list_elem, 'id', desc_id)
      self._SetAttribute(list_elem, 'key',
                        self._REGISTRY_KEY + '\\' + policy_name)
      self._SetAttribute(list_elem, 'valuePrefix', None)

      listbox_elem = self._AddElement(presentation_elem, 'listBox')
      self._SetAttribute(listbox_elem, 'refId', desc_id)
      listbox_elem.appendChild(self._adml_doc.createTextNode(policy_title))
    else:
      raise Exception('Unhandled schema type "%s"' % policy_schema['type'])


  def _ToPrettyXml(self, xml):
    # return doc.toprettyxml(indent='  ')
    # The above pretty-printer does not print the doctype and adds spaces
    # around texts, e.g.:
    #  <string>
    #    value of the string
    #  </string>
    # This is problematic both for the OSX Workgroup Manager (plist files) and
    # the Windows Group Policy Editor (admx files). What they need instead:
    #  <string>value of string</string>
    # So we use a hacky pretty printer here. It assumes that there are no
    # mixed-content nodes.
    # Get all the XML content in a one-line string.

    # Determine where the line breaks will be. (They will only be between tags.)
    lines = xml[1:len(xml) - 1].split('><')
    indent = ''
    # Determine indent for each line.
    for i, line in enumerate(lines):
      if line[0] == '/':
        # If the current line starts with a closing tag, decrease indent before
        # printing.
        indent = indent[2:]
      lines[i] = indent + '<' + line + '>'
      if (line[0] not in ['/', '?', '!'] and '</' not in line and
          line[len(line) - 1] != '/'):
        # If the current line starts with an opening tag and does not conatin a
        # closing tag, increase indent after the line is printed.
        indent += '  '
    # Reconstruct XML text from the lines.
    return '\n'.join(lines)


def ConvertJsonToAdmx(extension_id, extension_name, schema_file, admx_file,
                      adml_file):
  '''
  Loads the schema.json file |schema_file|, generates the ADMX and ADML docs and
  saves them to |admx_file| and |adml_file|, respectively. The name of the
  template and the registry keys are determined from the |extension_name| and
  the 32-byte |extension_id|, respectively.

  '''

  # Load that schema.
  with open(schema_file, 'r') as f:
    schema = eval(f.read())

  admx_generator = AdmxGenerator(extension_id, extension_name, schema)
  admx, adml = admx_generator.CreateTemplateXml()

  with open(admx_file, 'w') as f:
    f.write(admx)
  with open(adml_file, 'w') as f:
    f.write(adml)


def main():
  '''Main function, usage see top of file.'''
  parser = OptionParser(usage=__doc__)
  parser.add_option('--name', dest='extension_name',
                    help='extension name (e.g. Managed Bookmarks)')
  parser.add_option('--id', dest='extension_id',
                    help='extension id (e.g. gihmafigllmhbppdfjnfecimiohcljba)')
  parser.add_option('--schema', dest='schema_file',
                    help='Input schema.json file for the extension',
                    metavar='FILE')
  parser.add_option('--admx', dest='admx_file', help='Output ADMX file',
                    metavar='FILE')
  parser.add_option('--adml', dest='adml_file', help='Output ADML file',
                    metavar='FILE')
  (options, args) = parser.parse_args()

  if not options.extension_name or not options.extension_id or \
     not options.schema_file or not options.admx_file or not options.adml_file:
    parser.print_help()
    return 1

  ConvertJsonToAdmx(options.extension_name, options.extension_id,
                    options.schema_file, options.admx_file, options.adml_file)
  return 0


if __name__ == '__main__':
  sys.exit(main())
