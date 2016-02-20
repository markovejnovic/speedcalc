# -*- coding: utf-8 -*-
import bisect
from collections import deque
import re

from sphinx import addnodes
from sphinx.directives import ObjectDescription
from sphinx.domains import Domain, Index, ObjType, StandardDomain
from sphinx.roles import XRefRole
from sphinx.util.docfields import Field, GroupedField
from sphinx.util.nodes import make_refnode

from translations import load_translations, _, l_
import qtkeyword

_SIG_RE = re.compile(r'''
    ^                       # start
    (\w+)                   # function name
    \s*                     # optional whitespace
    (?: \( ([^)]*) \) )?    # optional argument list
    $                       # end
''', re.VERBOSE)


def sc_parameterlist(*args, **kwargs):
    node = addnodes.desc_parameterlist(*args, **kwargs)
    node.child_text_separator = '; '
    return node


def _parse_parameter_list(params):
    """Parse a SpeedCrunch parameter list into nodes."""
    paramlist = sc_parameterlist()
    stack = deque([paramlist])
    for param in params.split(';'):
        param = param.strip()
        if param and param[-1] in '[]':
            p = param[:-1].strip()
            stack[-1] += addnodes.desc_parameter(p, p)
        else:
            stack[-1] += addnodes.desc_parameter(param, param)

        if param.endswith('['):
            node = addnodes.desc_optional()
            stack[-1] += node
            stack.append(node)
        elif param.endswith(']'):
            stack.pop()

    return paramlist


class SpeedCrunchObject(ObjectDescription):
    """Directive to document a SpeedCrunch object."""

    doc_field_types = [
        # l10n: Label for parameter lists when documenting functions
        GroupedField('parameter', label=l_('Parameters'),
                     names=('param', 'parameter', 'arg', 'argument',)),
        # l10n: Label for the return value field when documenting functions
        Field('returnvalue', label=l_('Returns'), has_arg=False,
              names=('returns', 'return',)),
    ]

    needs_arglist = False

    def _needs_arglist(self):
        return self.needs_arglist

    def handle_signature(self, sig, signode):
        m = _SIG_RE.match(sig)
        if m is None:
            raise ValueError
        name, arglist = m.groups()
        signode += addnodes.desc_name(name, name)
        if not arglist and self._needs_arglist():
            signode += sc_parameterlist()
        elif arglist:
            signode += _parse_parameter_list(arglist)
        return name

    def get_index_text(self, name):
        """Get index display string for the given object name."""
        return name

    def add_target_and_index(self, name, sig, signode):
        targetname = 'sc.' + name
        if targetname not in self.state.document.ids:
            signode['names'].append(targetname)
            signode['ids'].append(targetname)
            signode['first'] = (not self.names)
            self.state.document.note_explicit_target(signode)
            inv = self.env.domaindata[self.domain]['objects']
            if name in inv:
                self.state_machine.reporter.warning(
                    'duplicate SpeedCrunch object description of %s, ' % name +
                    'other instance in ' + self.env.doc2path(inv[name][0]),
                    line=self.lineno)
            inv[name] = (self.env.docname, self.objtype)

        indextext = self.get_index_text(name)
        if indextext:
            self.indexnode['entries'].append(('single', indextext,
                                              targetname, None))

        qtkeyword.add_id_keyword(self.env, name, self.env.docname, targetname)


class SpeedCrunchFunction(SpeedCrunchObject):
    """Documents a SpeedCrunch function."""

    needs_arglist = True

    def get_index_text(self, name):
        # l10n: Index display text for built-in functions
        return _('%s() (function)') % name


class SpeedCrunchConstant(SpeedCrunchObject):
    """Documents a SpeedCrunch built-in constant."""

    def get_index_text(self, name):
        # l10n: Index display text for built-in constants
        return _('%s (constant)') % name


class FunctionIndex(Index):
    """Generate an index of all SpeedCrunch functions."""

    name = 'functionindex'
    # l10n: Function index long name (title and links)
    localname = l_('Function Index')
    # l10n: Function index short name (used in the header of some Sphinx themes)
    shortname = l_('functions')

    def generate(self, docnames=None):
        content = {}
        objs = sorted(self.domain.data['objects'].items())
        for name, (docname, objtype) in objs:
            if not name or objtype != 'function':
                continue
            entries = content.setdefault(name[0].lower(), [])
            e = (name, 0, docname, 'sc.' + name, '', '', '')
            bisect.insort_left(entries, e)
        return sorted(content.items()), False


class SpeedCrunchDomain(Domain):
    """Domain for documenting SpeedCrunch functions."""

    name = 'sc'
    label = 'SpeedCrunch'
    data_version = 1

    object_types = {
        # l10n: Label for built-in SpeedCrunch functions
        'function': ObjType(l_('function'), 'func'),
        # l10n: Label for built-in SpeedCrunch constants
        'constant': ObjType(l_('constant'), 'const'),
    }

    directives = {
        'function': SpeedCrunchFunction,
        'constant': SpeedCrunchConstant,
    }

    indices = [FunctionIndex]

    roles = {
        'func': XRefRole(fix_parens=True),
        'const': XRefRole(),
    }

    initial_data = {
        'objects': {},
    }

    def __init__(self, env):
        super(SpeedCrunchDomain, self).__init__(env)
        load_translations(env)

    def clear_doc(self, docname):
        for name, (i_docname, objtype) in list(self.data['objects'].items()):
            if i_docname == docname:
                del self.data['objects'][name]

    def merge_domaindata(self, docnames, otherdata):
        for name, (docname, objtype) in otherdata['objects'].items():
            if docname in docnames:
                self.data['objects'][name] = (docname, objtype)

    def resolve_xref(self, env, fromdocname, builder, typ, target, node,
                     contnode):
        try:
            docname, objtype = self.data['objects'][target]
        except KeyError:
            return None
        return make_refnode(builder, fromdocname, docname, 'sc.' + target,
                            contnode, target)

    def resolve_any_xref(self, env, fromdocname, builder, target, node,
                         contnode):
        try:
            docname, objtype = self.data['objects'][target]
        except KeyError:
            return None
        return [('sc:' + self.role_for_objtype(objtype),
                 make_refnode(builder, fromdocname, docname, 'sc.' + target,
                              contnode, target))]

    def get_objects(self):
        for name, (docname, objtype) in self.data['objects'].items():
            yield (name, name, objtype, docname, 'sc.' + name, 1)


def setup(app):
    app.add_domain(SpeedCrunchDomain)
    # workaround so we can include our custom index via sc:functionindex
    StandardDomain.initial_data['labels']['sc:functionindex'] = \
        ('sc-functionindex', '', FunctionIndex.localname)
    StandardDomain.initial_data['anonlabels']['sc:functionindex'] = \
        ('sc-functionindex', '')
    return {'version': '0.1'}
